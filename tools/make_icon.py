#!/usr/bin/env python3
"""生成 UseStats 的扁平风图标 app.ico（黄底 + 黑色粗线时钟 + 紫色秒针）。

在 1024x1024 画布上以 4 倍超采样绘制，再逐级缩小导出多尺寸 ICO，
保证 16x16 小尺寸下依然可辨认。只需运行一次，产物 app.ico 提交在工程根目录。
"""
from PIL import Image, ImageDraw

S = 1024
YELLOW = (255, 199, 0, 255)
PURPLE = (108, 76, 241, 255)
BLACK = (20, 20, 20, 255)

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# 黄色圆角方形底（切角感：右下角切掉一小块，呼应界面的斜切几何）
r = 190
d.rounded_rectangle([28, 28, S - 28, S - 28], radius=r, fill=YELLOW)
cut = 150
d.polygon([(S - 28 - cut, S - 28), (S - 28, S - 28 - cut), (S - 28, S - 28)], fill=(0, 0, 0, 0))

# 时钟外圈（粗黑环）
cx = cy = S // 2
R = 330
ring = 64
d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=BLACK, width=ring)
# 刻度（12/3/6/9 四个主刻度）
import math

for ang in (0, 90, 180, 270):
    a = math.radians(ang)
    x1 = cx + (R - ring - 16) * math.cos(a)
    y1 = cy + (R - ring - 16) * math.sin(a)
    x2 = cx + (R - ring - 92) * math.cos(a)
    y2 = cy + (R - ring - 92) * math.sin(a)
    d.line([x1, y1, x2, y2], fill=BLACK, width=44)
# 时针（指向 10 点）与分针（指向 2 点），粗黑线
for ang, length, w in ((300, 190, 58), (60, 262, 46)):
    a = math.radians(ang)
    d.line([cx, cy, cx + length * math.cos(a), cy + length * math.sin(a)], fill=BLACK, width=w)
# 紫色秒针（指向 6 点半方向）+ 中心点
a = math.radians(160)
d.line([cx, cy, cx + 285 * math.cos(a), cy + 285 * math.sin(a)], fill=PURPLE, width=26)
d.ellipse([cx - 46, cy - 46, cx + 46, cy + 46], fill=BLACK)
d.ellipse([cx - 20, cy - 20, cx + 20, cy + 20], fill=PURPLE)

img.save(
    "../app.ico",
    format="ICO",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
)
print("app.ico written")
