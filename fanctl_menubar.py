#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fanctl 菜单栏 App — Apple M4 Mac mini 风扇转速监控 + 温度阈值控制
- 菜单栏实时显示: 转速 rpm + 最高温度 °C
- 规则: 温度 ≤ low_temp → low_speed; 温度 ≥ high_temp → high_speed; 之间线性
- 写操作经 sudo -n fanctl-smc（sudoers 免密）
- 配置持久化 ~/Library/Application Support/fanctl/config.json
"""
import json
import os
import subprocess
import threading
import time

import rumps

import objc
import AppKit
import Foundation
from AppKit import NSStringFromRect

APP_NAME = "fanctl"
TOOL = os.path.expanduser("~/Projects/fanctl/fanctl-smc")
CONFIG_DIR = os.path.expanduser("~/Library/Application Support/fanctl")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")

DEFAULT_CONFIG = {
    "enabled": False,          # 温控规则总开关
    "low_temp": 65.0,          # 低温阈值 °C
    "low_speed": 1200,         # 低温时转速 rpm
    "high_temp": 80.0,         # 高温阈值 °C
    "high_speed": 3500,        # 高温时转速 rpm
    "interval": 5,             # 刷新间隔秒
}

REFRESH_INTERVAL = 5


def log(msg):
    try:
        with open("/tmp/fanctl-menubar.log", "a") as f:
            f.write("%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
    except Exception:
        pass


def load_config():
    cfg = dict(DEFAULT_CONFIG)
    try:
        with open(CONFIG_PATH) as f:
            cfg.update(json.load(f))
    except Exception:
        pass
    return cfg


def save_config(cfg):
    try:
        os.makedirs(CONFIG_DIR, exist_ok=True)
        with open(CONFIG_PATH, "w") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception as e:
        log("save_config error: %s" % e)


def run_tool(args, sudo=False):
    """调用 fanctl-smc，返回 stdout 或 None"""
    cmd = ["sudo", "-n", TOOL] + args if sudo else [TOOL] + args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        if r.returncode == 0:
            return r.stdout.strip()
        log("tool error %s: %s" % (args, r.stderr.strip()[:200]))
    except Exception as e:
        log("run_tool exception %s: %s" % (args, e))
    return None


class FanStatusView(AppKit.NSView):
    """自定义状态栏视图：温度/转速两行 label 组成一个垂直组合，
    组合的垂直中心与状态栏（菜单栏）中心对齐（参考 State app 布局）。
    """

    def initWithFrame_(self, frame):
        self = objc.super(FanStatusView, self).initWithFrame_(frame)
        if self is None:
            return None
        self._menu = None
        self._status_item = None

        # 第一行：温度（着色）
        self._temp_label = AppKit.NSTextField.labelWithString_("--°")
        self._temp_label.setFont_(
            AppKit.NSFont.monospacedDigitSystemFontOfSize_weight_(9.0, AppKit.NSFontWeightSemibold))
        self._temp_label.setAlignment_(AppKit.NSTextAlignmentCenter)
        self._temp_label.setBordered_(False)
        self._temp_label.setDrawsBackground_(False)
        self._temp_label.setEditable_(False)
        self._temp_label.setSelectable_(False)

        # 第二行：转速
        self._rpm_label = AppKit.NSTextField.labelWithString_("----")
        self._rpm_label.setFont_(
            AppKit.NSFont.monospacedDigitSystemFontOfSize_weight_(8.0, AppKit.NSFontWeightRegular))
        self._rpm_label.setAlignment_(AppKit.NSTextAlignmentCenter)
        self._rpm_label.setBordered_(False)
        self._rpm_label.setDrawsBackground_(False)
        self._rpm_label.setEditable_(False)
        self._rpm_label.setSelectable_(False)

        # 垂直组合：NSStackView 自动按内容排布两行
        self._stack = AppKit.NSStackView.alloc().initWithFrame_(
            Foundation.NSMakeRect(0, 0, 40, 20))
        self._stack.setOrientation_(AppKit.NSUserInterfaceLayoutOrientationVertical)
        self._stack.setSpacing_(0.0)
        self._stack.setAlignment_(AppKit.NSLayoutAttributeCenterX)
        self._stack.addView_inGravity_(self._temp_label, AppKit.NSStackViewGravityTop)
        self._stack.addView_inGravity_(self._rpm_label, AppKit.NSStackViewGravityTop)
        self._stack.setTranslatesAutoresizingMaskIntoConstraints_(False)
        self.addSubview_(self._stack)

        # 组合的中心 = 状态栏视图的中心（这正是"两排文本组合垂直居中"的实现）
        self._stack.centerXAnchor().constraintEqualToAnchor_(self.centerXAnchor()).setActive_(True)
        self._stack.centerYAnchor().constraintEqualToAnchor_(self.centerYAnchor()).setActive_(True)
        return self

    def mouseDown_(self, event):
        # 点击弹出菜单（view 模式不会自动弹菜单）
        if self._menu is not None and self._status_item is not None:
            self._status_item.popUpStatusItemMenu_(self._menu)
        else:
            objc.super(FanStatusView, self).mouseDown_(event)

    @objc.python_method
    def _update(self, temperature, rpm):
        self._temp_label.setStringValue_("%.0f°" % temperature)
        self._rpm_label.setStringValue_("%d rpm" % rpm)
        if temperature > 90:
            color = AppKit.NSColor.systemRedColor()
        elif temperature > 70:
            color = AppKit.NSColor.systemOrangeColor()
        else:
            color = AppKit.NSColor.labelColor()
        self._temp_label.setTextColor_(color)


class FanctlApp(rumps.App):
    def __init__(self):
        super().__init__("…", quit_button=None)
        self.cfg = load_config()
        self.fan_info = None      # 风扇状态
        self.hot_temp = 0.0       # 最高温度
        self.hot_key = "-"
        self.current_target = None  # 当前已应用的目标转速
        self.current_mode = None    # 当前 SMC 模式 0/1
        self.sudo_ok = False
        self.temp_ema = None        # 温度 EMA 平滑
        self.last_write_ts = 0.0    # 上次实际写 SMC 的时间
        self.last_target = None     # 上次计算的目标（状态同步用）
        self.last_apply_temp = None # 上次实际写入时的温度（滞回基准）
        self.temp_max = 0.0         # 最高稳定温度（显示用）

        self.status_item = rumps.MenuItem("状态: 读取中…")
        self.status_item.set_callback(None)
        self.mode_item = rumps.MenuItem("SMC 模式: -")
        self.mode_item.set_callback(None)
        self.apply_item = rumps.MenuItem("▶ 立即应用温控规则")
        self.apply_item.set_callback(lambda _: self.apply_rule(force=True))
        self.menu = [
            self.status_item,
            self.mode_item,
            None,
            self.apply_item,
            rumps.MenuItem("⚙ 温度阈值设置…", callback=lambda _: self.edit_config()),
            rumps.MenuItem("🔄 刷新", callback=lambda _: self.refresh()),
            None,
            rumps.MenuItem("🚪 退出", callback=lambda _: rumps.quit_application()),
        ]

        self._fan_view = None  # 自定义状态栏视图（run() 后创建）
        self.refresh()
        threading.Timer(0.1, self._hide_dock).start()
        threading.Thread(target=self._loop, daemon=True).start()

    def _hide_dock(self):
        import AppKit
        AppKit.NSApp.setActivationPolicy_(AppKit.NSApplicationActivationPolicyAccessory)

    def _loop(self):
        """主循环：任何异常不得杀死线程（未捕获异常曾导致 17 分钟温控空窗）"""
        while True:
            try:
                interval = self.cfg.get("interval", REFRESH_INTERVAL)
                if not isinstance(interval, (int, float)) or interval <= 0:
                    interval = REFRESH_INTERVAL
                time.sleep(interval)
                self.refresh()
                if self.cfg.get("enabled"):
                    self.apply_rule(force=False)
            except Exception as e:
                log("loop exception (kept alive): %s" % e)
                time.sleep(2)

    # ---------- 数据 ----------

    def refresh(self):
        # 热加载配置（支持外部修改 config.json / GUI 保存后生效）
        self.cfg = load_config()
        out = run_tool(["fans"])
        if out is not None:
            for line in out.splitlines():
                if line.startswith("fan "):
                    p = dict()
                    for kv in line.split()[2:]:
                        k, v = kv.split("=")
                        p[k] = float(v)
                    self.fan_info = p
        out = run_tool(["temp"])
        if out is not None:
            for line in out.splitlines():
                if line.startswith("temp "):
                    parts = line.split()
                    self.hot_key = parts[1]
                    self.hot_temp = float(parts[2])
                elif line.startswith("temp2 "):
                    parts = line.split()
                    self.temp_max = float(parts[2])
            # EMA 平滑：消除单次读数的瞬时尖峰
            if self.temp_ema is None:
                self.temp_ema = self.hot_temp
            else:
                self.temp_ema = self.temp_ema * 0.7 + self.hot_temp * 0.3
        # sudo 可用性探测（用白名单内的命令，sudo -n true 不在 sudoers 白名单会误判）
        if not self.sudo_ok:
            r = subprocess.run(["sudo", "-n", TOOL, "get", "F0Md"],
                               capture_output=True, timeout=3)
            self.sudo_ok = (r.returncode == 0)
            if not self.sudo_ok:
                log("sudo probe failed: %s" % r.stderr.strip()[:120])
        self.update_ui()

    def update_ui(self):
        fan = self.fan_info or {}
        cur = fan.get("cur", 0)
        mode = int(fan.get("mode", -1))
        self.current_mode = mode
        disp_temp = self.temp_ema if self.temp_ema is not None else self.hot_temp
        # 全部 UI 操作（含 rumps MenuItem title）必须回主线程：
        # 菜单弹出时后台线程 setTitle 会触发 NSMenuItem setTitle: → 弹出窗口重排 → 断言崩溃
        import dispatch
        dispatch.dispatch_async(
            dispatch.dispatch_get_main_queue(),
            lambda: self._update_ui_main(disp_temp, cur, mode))

    @objc.python_method
    def _update_ui_main(self, disp_temp, cur, mode):
        try:
            self._set_status_title(disp_temp, cur)
        except Exception as e:
            log("ui title exception: %s" % e)
        try:
            mode_txt = {0: "自动", 1: "手动"}.get(mode, "未知")
            self.status_item.title = "转速 %d rpm   温度 %s %.1f°C (平滑)" % (
                cur, self.hot_key, disp_temp)
            rule = "开" if self.cfg.get("enabled") else "关"
            target = self.current_target if self.current_target is not None else "-"
            self.mode_item.title = "SMC 模式: %s   温控: %s   目标: %s" % (
                mode_txt, rule, target)
            if not self.sudo_ok:
                self.mode_item.title += "   ⚠ 无写权限"
        except Exception as e:
            log("ui menu exception: %s" % e)

    def _set_status_title(self, temperature, rpm):
        """两行显示：自定义 NSView（参考 State app 布局），温度着色，整体垂直居中。"""
        import AppKit

        try:
            nsapp = getattr(self, "_nsapp", None)
            if nsapp is None:
                return  # run() 之前的首次 refresh，静默跳过
            item = nsapp.nsstatusitem
            if item is None:
                return
            import dispatch
            main_q = dispatch.dispatch_get_main_queue()
            if self._fan_view is None:
                # 创建 view + setView_ 必须在主线程；已在主线程则直接调用（避免 sync 死锁）
                if AppKit.NSThread.isMainThread():
                    self._create_view(item)
                else:
                    dispatch.dispatch_sync(main_q, lambda: self._create_view(item))
            # UI 更新回主线程（已在主线程时直接执行）
            if AppKit.NSThread.isMainThread():
                self._fan_view._update(temperature, rpm)
            else:
                dispatch.dispatch_async(main_q,
                                        lambda: self._fan_view._update(temperature, rpm))
        except AttributeError as e:
            log("STATUS exception: %s" % e)
        except Exception as e:
            log("STATUS exception2: %s" % e)

    @objc.python_method
    def _create_view(self, item):
        try:
            view = FanStatusView.alloc().initWithFrame_(
                Foundation.NSMakeRect(0, 0, 52, 22))
            view._menu = self.menu._menu
            view._status_item = item
            item.setView_(view)
            self._fan_view = view
        except Exception as e:
            log("create_view exception: %s" % e)

    # ---------- 规则 ----------

    def apply_rule(self, force=False):
        if not self.cfg.get("enabled"):
            return
        lo_t = self.cfg.get("low_temp", 65)
        hi_t = self.cfg.get("high_temp", 80)
        lo_s = int(self.cfg.get("low_speed", 1200))
        hi_s = int(self.cfg.get("high_speed", 3500))
        fan = self.fan_info or {}
        fmin = fan.get("min", 1000)
        fmax = fan.get("max", 4900)
        # 用 EMA 平滑温度（TCMz 原始读数抖动 ±17°C 会导致转速乱跳）
        t = self.temp_ema if self.temp_ema is not None else self.hot_temp
        if t <= lo_t:
            target = lo_s
        elif t >= hi_t:
            target = hi_s
        else:
            r = (t - lo_t) / max(hi_t - lo_t, 0.1)
            target = int(lo_s + (hi_s - lo_s) * r)
        target = max(int(fmin), min(int(fmax), target))
        # 心跳日志（60s 一条）：任何空窗/停摆可精确定位是循环死还是短路
        now_hb = time.time()
        if now_hb - getattr(self, "_hb_ts", 0) >= 60:
            self._hb_ts = now_hb
            log("heartbeat: t=%.1f target=%d applied=%s last_target=%s last_apply_temp=%s mode=%s enabled=%s" % (
                t, target, self.current_target, self.last_target,
                getattr(self, "last_apply_temp", None), self.current_mode,
                self.cfg.get("enabled")))
        # 滞回（基于温度）：距上次实际写入温度变化 < 1.0°C 不写。
        # 温度持续变化时每 1°C 至少写一次，杜绝"target 卡滞回窗口"型空窗；
        # 温度稳定时静默（省 SMC 写）。短路时同步 last_target 保持状态一致。
        if (not force and getattr(self, "last_apply_temp", None) is not None
                and abs(t - self.last_apply_temp) < 1.0):
            self.last_target = target
            return
        # 写节流：距上次实际写 < 10s 不重复写
        now = time.time()
        if not force and now - self.last_write_ts < 10:
            self.last_target = target
            return
        if self.current_target == target and self.current_mode == 1:
            self.last_target = target
            return
        # 写操作: 手动模式 + 目标转速
        if self.sudo_ok:
            out = run_tool(["fan", "-m", "1", "-v", str(target)], sudo=True)
            if out is not None:
                self.current_target = target
                self.current_mode = 1
                self.last_write_ts = time.time()
                self.last_target = target
                self.last_apply_temp = t
                log("apply rule: t=%.1f -> %d rpm" % (t, target))
        self.update_ui()

    def reset_auto(self):
        if self.sudo_ok:
            run_tool(["reset"], sudo=True)
            self.current_target = None
            self.current_mode = 0
            log("reset to auto")
        self.refresh()

    # ---------- 配置 UI（单页多字段 NSAlert） ----------

    def edit_config(self):
        import AppKit
        from AppKit import NSAlert, NSTextField, NSView
        from Foundation import NSMakeRect

        cfg = self.cfg
        alert = NSAlert.alloc().init()
        alert.setMessageText_("温度阈值设置")
        alert.setInformativeText_("温度 ≤ 低温 → 低转速；温度 ≥ 高温 → 高转速；之间线性过渡。写风扇需管理员授权。")
        alert.addButtonWithTitle_("保存")
        alert.addButtonWithTitle_("取消")

        view = NSView.alloc().initWithFrame_(NSMakeRect(0, 0, 380, 210))
        fields = {}

        def add_row(y, label, key, hint=""):
            lb = NSTextField.labelWithString_(label)
            lb.setFrame_(NSMakeRect(0, y, 150, 20))
            view.addSubview_(lb)
            f = NSTextField.alloc().initWithFrame_(NSMakeRect(158, y - 2, 90, 24))
            f.setStringValue_(str(cfg.get(key, "")))
            view.addSubview_(f)
            fields[key] = f
            if hint:
                hb = NSTextField.labelWithString_(hint)
                hb.setFrame_(NSMakeRect(256, y, 124, 20))
                hb.setTextColor_(AppKit.NSColor.secondaryLabelColor())
                hb.setFont_(AppKit.NSFont.systemFontOfSize_(10.0))
                view.addSubview_(hb)

        add_row(170, "低温阈值 °C", "low_temp", "≤ 此温度")
        add_row(132, "低温转速 rpm", "low_speed")
        add_row(94, "高温阈值 °C", "high_temp", "≥ 此温度")
        add_row(56, "高温转速 rpm", "high_speed")
        add_row(18, "刷新间隔 秒", "interval")

        alert.setAccessoryView_(view)
        alert.window().setInitialFirstResponder_(fields["low_temp"])
        AppKit.NSApp.activateIgnoringOtherApps_(True)

        if alert.runModal() == 1000:
            try:
                cfg["low_temp"] = float(fields["low_temp"].stringValue())
                cfg["low_speed"] = float(fields["low_speed"].stringValue())
                cfg["high_temp"] = float(fields["high_temp"].stringValue())
                cfg["high_speed"] = float(fields["high_speed"].stringValue())
                cfg["interval"] = max(2, int(float(fields["interval"].stringValue())))
                if cfg["high_temp"] <= cfg["low_temp"]:
                    raise ValueError("高温阈值必须大于低温阈值")
                if cfg["high_speed"] < cfg["low_speed"]:
                    raise ValueError("高温转速应不低于低温转速")
                self.cfg = cfg
                save_config(cfg)
                log("config saved: %s" % cfg)
                self.refresh()
            except ValueError as e:
                rumps.alert(title="设置无效", message=str(e), ok="知道了")
            except Exception as e:
                rumps.alert(title="设置无效", message="请检查输入数字: %s" % e, ok="知道了")


if __name__ == "__main__":
    FanctlApp().run()
