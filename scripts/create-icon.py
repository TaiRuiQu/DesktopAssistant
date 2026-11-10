"""Generate the code-drawn application mark; Pillow is needed only to regenerate it."""
from pathlib import Path
from PIL import Image, ImageDraw

size = 1024
image = Image.new("RGBA", (size, size))
draw = ImageDraw.Draw(image)
draw.rounded_rectangle((0, 0, 1023, 1023), radius=250, fill="#0067C0")
draw.polygon([(512, 165), (609, 415), (859, 512), (609, 609),
              (512, 859), (415, 609), (165, 512), (415, 415)], fill="white")
destination = Path(__file__).resolve().parents[1] / "src/DesktopAssistant/Assets/app.ico"
destination.parent.mkdir(parents=True, exist_ok=True)
image.resize((256, 256), Image.Resampling.LANCZOS).save(destination, sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
