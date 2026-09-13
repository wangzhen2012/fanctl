#!/usr/bin/env python3
import rumps
import threading, time

class MiniApp(rumps.App):
    def __init__(self):
        super().__init__("MiniTest", title="TEST123", quit_button=None)
        self.menu = [rumps.MenuItem("hello")]
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            time.sleep(2)
            self.title = "TEST%d" % int(time.time() % 100)

if __name__ == "__main__":
    MiniApp().run()
