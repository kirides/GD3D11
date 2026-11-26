"""Whole-desktop (or one window) screenshot via PIL, for diagnosing the automation itself."""
import sys
from PIL import ImageGrab
ImageGrab.grab(all_screens=True).save(sys.argv[1] if len(sys.argv) > 1 else 'desk.png')
