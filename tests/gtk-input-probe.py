from pathlib import Path
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk


def save_text(entry, output):
    output.write_text(entry.get_text(), encoding="utf-8")


output = Path(sys.argv[1])
window = Gtk.Window(title="Fcitx5 input probe")
window.set_default_size(600, 100)
window.connect("destroy", Gtk.main_quit)
entry = Gtk.Entry()
entry.connect("changed", save_text, output)
window.add(entry)
window.show_all()
entry.grab_focus()
Gtk.main()
