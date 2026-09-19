# 초원 · 구름 · 나무 v2 — 더 멋지게. 헤드리스 연산 생성기.
import json, math, random
random.seed(11)
W, H = 1600, 1000
ops = []
cur = [None]
def op(o):
    if o['op'] == 'layer.add': cur[0] = o['name']
    if o['op'] in ('stroke', 'gradient', 'fill', 'bucket', 'erase') and 'layer' not in o and cur[0]: o['layer'] = cur[0]
    ops.append(o)
def stroke(brush, size, color, pts, opacity=None, profile="flat", **kw):
    o = {"op": "stroke", "brush": brush, "size": size, "color": color, "pressureProfile": profile,
         "points": [{"x": x, "y": y} if len(p) == 2 else {"x": p[0], "y": p[1], "p": p[2]} for p in pts for (x, y) in [(p[0], p[1])]]}
    if opacity is not None: o["opacity"] = opacity
    o.update(kw); op(o)
def dab(brush, size, color, x, y, opacity=None):
    stroke(brush, size, color, [(x, y), (x + 0.5, y)], opacity)
def lerp(a, b, t): return a + (b - a) * t
def hexmix(c1, c2, t):
    a = [int(c1[i:i+2], 16) for i in (1, 3, 5)]; b = [int(c2[i:i+2], 16) for i in (1, 3, 5)]
    return "#%02X%02X%02X" % tuple(int(lerp(a[i], b[i], t)) for i in range(3))

op({"op": "doc.create", "width": W, "height": H})

# ── 커스텀 붓: 풀붓(잉크펜 + 색 지터) ──
op({"op": "brush.export", "brush": "잉크펜"})   # 참고용(결과는 안 씀)
grass_brush = {"format": "mari-brush", "version": 1, "name": "풀붓", "sourceFormat": "native", "engine": "",
               "tip": {"kind": "procedural", "shape": "circle", "diameter": 3, "angle": 0, "aspectRatio": 0.6, "hardness": 0.85},
               "spacing": 0.12, "opacity": 1.0, "flow": 1.0, "blendMode": "normal",
               "dynamics": [{"input": "pressure", "output": "size", "amount": 1, "curve": [[0, 0.15], [1, 1]]},
                            {"input": "direction", "output": "rotation", "amount": 1, "curve": [[0, 0], [1, 360]]}],
               "colorDynamics": {"fgBgJitter": 0, "hueJitter": 0.04, "saturationJitter": 0.12, "brightnessJitter": 0.10, "purity": 0, "perTip": False},
               "scatterCount": 1, "wetEdges": False, "noise": 0, "airbrush": False, "extraParams": {}}
op({"op": "brush.save", "preset": grass_brush})
leaf_brush = dict(grass_brush); leaf_brush = json.loads(json.dumps(grass_brush))
leaf_brush["name"] = "잎붓"; leaf_brush["tip"] = {"kind": "procedural", "shape": "circle", "diameter": 20, "angle": 30, "aspectRatio": 0.55, "hardness": 0.7}
leaf_brush["spacing"] = 0.5; leaf_brush["opacity"] = 0.92
leaf_brush["dynamics"] = [{"input": "random", "output": "rotation", "amount": 1, "curve": [[0, 0], [1, 70]]}, {"input": "random", "output": "size", "amount": 1, "curve": [[0, 0.6], [1, 1]]}]
leaf_brush["colorDynamics"] = {"fgBgJitter": 0, "hueJitter": 0.04, "saturationJitter": 0.15, "brightnessJitter": 0.18, "purity": 0, "perTip": True}
op({"op": "brush.save", "preset": leaf_brush})

# ── 하늘 ──
op({"op": "layer.setProps", "name": "하늘"}); cur[0] = "하늘"
op({"op": "gradient", "region": "canvas", "from": "#3F7FD1", "to": "#BFE0FA", "angle": 90})
op({"op": "layer.add", "name": "지평선 빛"})
op({"op": "select", "mode": "rect", "region": [0, 470, W, 250]})
op({"op": "select.feather", "radius": 50})
op({"op": "gradient", "from": [255, 232, 200, 0], "to": [255, 226, 190, 70], "angle": 90})
op({"op": "select"})
# 해
op({"op": "layer.add", "name": "해"})
for d, o, c in [(1100, 0.16, "#FFF4D6"), (700, 0.2, "#FFF6DC"), (380, 0.35, "#FFFBEA"), (170, 0.9, "#FFFFF4")]:
    dab("에어브러시", d, c, 1350, 120, o)
op({"op": "layer.setProps", "layer": "해", "blendMode": "screen"})

# ── 먼 산 두 겹 ──
def ridge(name, y0, amp, seed, c_top, c_bot, yend):
    random.seed(seed)
    op({"op": "layer.add", "name": name})
    pts = [[0, y0]]
    x = 0
    while x < W:
        x += random.randint(50, 130)
        pts.append([x, y0 - random.uniform(0, amp) - amp * 0.5 * math.sin(x / 300 + seed)])
    pts += [[W, y0], [W, yend], [0, yend]]
    op({"op": "select", "mode": "lasso", "points": pts, "antialias": True})
    op({"op": "gradient", "from": c_top, "to": c_bot, "angle": 90})
    op({"op": "select"})
ridge("먼 산 2", 560, 150, 3, "#9DBBD9", "#7C9FC4", 720)
ridge("먼 산 1", 600, 110, 5, "#86A9CB", "#5F86B0", 740)
op({"op": "layer.setProps", "layer": "먼 산 2", "opacity": 0.75})
# 안개
op({"op": "layer.add", "name": "안개"})
# 🔴 에어브러시 긴 획은 스탬프가 겹쳐 쌓인다(불투명도 0.1 이어도 수십 번 겹치면 1). 넓은 안개는 선택+페더+채우기로.
op({"op": "select", "mode": "rect", "region": [0, 600, W, 90]})
op({"op": "select.feather", "radius": 40})
op({"op": "fill", "color": [228, 238, 248, 70]})
op({"op": "select"})

# ── 언덕 세 겹 ──
def hill(name, base, amp, phase, c_top, c_bot, shade):
    op({"op": "layer.add", "name": name})
    pts = [[0, H]]
    for x in range(0, W + 1, 32):
        pts.append([x, base + amp * math.sin(x / 420 + phase) + amp * 0.35 * math.sin(x / 130 + phase * 2)])
    pts += [[W, H]]
    op({"op": "select", "mode": "lasso", "points": pts, "antialias": True})
    op({"op": "gradient", "from": c_top, "to": c_bot, "angle": 90})
    op({"op": "select"})
hill("언덕 뒤", 655, 35, 0.4, "#A6D66E", "#6FB04E", "#3E7A33")
hill("언덕 중", 780, 45, 2.2, "#8CCB5C", "#4F9A3C", "#2F6E2B")
hill("언덕 앞", 870, 40, 4.1, "#6FBA48", "#2E7A2E", "#1F5A22")

# 먼 나무들(언덕 뒤 능선 위 작은 덩어리)
op({"op": "layer.add", "name": "먼 나무"})
random.seed(21)
for i in range(26):
    x = random.uniform(0, W)
    y = 655 + 35 * math.sin(x / 420 + 0.4) + 35 * 0.35 * math.sin(x / 130 + 0.8) + 4
    sz = random.uniform(10, 22)
    for k, c in [(1.0, "#3D7A3A"), (0.7, "#4E9448"), (0.4, "#6FB35C")]:
        dab("에어브러시", sz * k * 2.2, c, x + (1 - k) * 3, y - sz * 0.9 * k, 0.9)
    stroke("잉크펜", 2, "#3A2A1C", [(x, y + 2), (x, y - sz * 0.5)])

# ── 길 ──
op({"op": "layer.add", "name": "길"})
left, right = [], []
for t in range(0, 21):
    u = t / 20
    y = lerp(H + 40, 790, u)
    cx = 980 + 260 * math.sin(u * 3.0) * (1 - u) ** 0.6 - 300 * u
    wdt = lerp(150, 10, u ** 0.8)
    left.append([cx - wdt, y]); right.append([cx + wdt, y])
op({"op": "select", "mode": "lasso", "points": left + right[::-1], "antialias": True})
op({"op": "select.feather", "radius": 4})
op({"op": "gradient", "from": "#D9C39A", "to": "#B89A6E", "angle": 90})
op({"op": "select"})
# 길 가장자리 흙/풀 섞기
for i in range(80):
    u = random.uniform(0.02, 0.95)
    y = lerp(H + 40, 790, u); cx = 980 + 260 * math.sin(u * 3.0) * (1 - u) ** 0.6 - 300 * u; wdt = lerp(150, 10, u ** 0.8)
    side = random.choice([-1, 1])
    dab("에어브러시", random.uniform(14, 40) * (1 - u * 0.7), random.choice(["#C7AE85", "#A98B62", "#8FB35A"]), cx + side * wdt * random.uniform(0.7, 1.1), y, 0.5)

# ── 풀(풀붓, 색 지터) ──
op({"op": "layer.add", "name": "풀"})
random.seed(33)
def hill_y(x, base, amp, phase): return base + amp * math.sin(x / 420 + phase) + amp * 0.35 * math.sin(x / 130 + phase * 2)
for i in range(1400):
    x = random.uniform(-20, W + 20)
    y = random.uniform(655, H + 20)
    if y < hill_y(x, 655, 35, 0.4) + 4: continue
    depth = (y - 700) / 300
    h = random.uniform(6, 14) * (0.4 + depth * 1.6)
    lean = random.uniform(-0.5, 0.5) * h
    col = random.choice(["#7CC44F", "#5AA63C", "#3D8B34", "#2B6E2B", "#93D667"])
    stroke("풀붓", 2.0 + depth * 4.0, col, [(x, y, 1.0), (x + lean * 0.5, y - h * 0.6, 0.7), (x + lean, y - h, 0.15)])

# ── 구름 ──
op({"op": "layer.add", "name": "구름"})
def cloud(cx, cy, scale, flat=0.55):
    random.seed(int(cx))
    blobs = []
    for i in range(16):
        a = random.uniform(0, 2 * math.pi); r = math.sqrt(random.uniform(0, 1))
        blobs.append((math.cos(a) * r * 1.7, math.sin(a) * r * flat, random.uniform(0.55, 1.0)))
    for i in range(6):
        blobs.append((random.uniform(-1.2, 1.2), random.uniform(-1.0, -0.5), random.uniform(0.75, 1.0)))
    blobs.sort(key=lambda b: b[1])
    for bx, by, r in blobs:
        x0, y0 = cx + bx * 100 * scale, cy + by * 60 * scale
        for col, o, k in [("#DCE7F4", 0.32, 1.35), ("#F5F8FE", 0.6, 1.0), ("#FFFFFF", 0.9, 0.58)]:
            d = 115 * scale * r * k
            stroke("에어브러시", d, col, [(x0 - 8 * scale, y0), (x0 + 8 * scale, y0 - d * 0.04)], o)
    stroke("에어브러시", 130 * scale, "#A7BFDD", [(cx - 150 * scale, cy + 42 * scale), (cx + 150 * scale, cy + 44 * scale)], 0.25)
    stroke("에어브러시", 100 * scale, "#FFF9EA", [(cx + 30 * scale, cy - 58 * scale), (cx + 130 * scale, cy - 36 * scale)], 0.4)
cloud(330, 170, 1.15)
cloud(820, 120, 1.5)
cloud(1180, 230, 0.9)
cloud(560, 300, 0.6)
cloud(1450, 320, 0.5)

# ── 나무 ──
def tree(x, base, size, seed):
    random.seed(seed)
    trunk_h = 170 * size
    # 줄기: 여러 획으로 두께·결
    for k, c, w in [(0, "#4A2E1B", 1.0), (1, "#6B4526", 0.55), (2, "#3A2213", 0.3)]:
        stroke("잉크펜", 18 * size * w, c, [(x + k * 2 * size, base, 1.0), (x + 6 * size, base - trunk_h * 0.55, 0.75), (x + 3 * size, base - trunk_h, 0.4)], profile="flat")
    branches = []
    for ang in (-1.1, 0.95, -0.45, 0.5, 0.05):
        y0 = base - trunk_h * random.uniform(0.6, 0.9)
        ex, ey = x + math.sin(ang) * 85 * size, y0 - 75 * size + abs(math.sin(ang)) * 20 * size
        stroke("잉크펜", 7 * size, "#4A2E1B", [(x + 3 * size, y0, 0.9), (x + math.sin(ang) * 45 * size, y0 - 40 * size, 0.6), (ex, ey, 0.25)])
        branches.append((ex, ey))
    branches.append((x + 3 * size, base - trunk_h - 30 * size))
    # 잎 덩어리: 가지 끝마다, 어두운 → 밝은, 빛은 오른쪽 위
    for col, rad, n, d, ox, oy in [("#173F19", 1.0, 60, 30, -4, 10), ("#25602A", 0.92, 60, 26, 0, 4), ("#3C8A38", 0.82, 50, 21, 6, -4), ("#67B84D", 0.62, 34, 16, 13, -11), ("#A8E37A", 0.42, 18, 12, 20, -18), ("#E1FBBE", 0.24, 8, 9, 26, -24)]:
        for (bx, by) in branches:
            for i in range(n):
                a = random.uniform(0, 2 * math.pi); r = math.sqrt(random.uniform(0, 1)) * 48 * size * rad
                px, py = bx + math.cos(a) * r * 1.15 + ox * size, by + math.sin(a) * r * 0.75 + oy * size
                dab("잎붓", d * size, col, px, py)
tree(360, 905, 1.25, 1)
tree(1180, 830, 0.9, 2)
tree(1500, 900, 0.7, 3)
# 덤불(길 옆)
random.seed(77)
for (bx, by, s_) in [(760, 960, 0.6), (600, 985, 0.5), (1000, 900, 0.45), (1300, 935, 0.5)]:
    for col, rad, n, d in [("#1D4F22", 1.0, 60, 40), ("#2F7A32", 0.85, 50, 32), ("#5FAE45", 0.6, 34, 24), ("#A3DC72", 0.35, 14, 16)]:
        for i in range(n):
            a = random.uniform(0, 2 * math.pi); r = math.sqrt(random.uniform(0, 1)) * 60 * s_ * rad
            dab("잎붓", d * s_, col, bx + math.cos(a) * r * 1.3, by + math.sin(a) * r * 0.6 - (1 - rad) * 12 * s_)

# 그림자(multiply)
op({"op": "layer.add", "name": "그림자", "index": 9})
for x, base, size in [(360, 905, 1.25), (1180, 830, 0.9), (1500, 900, 0.7)]:
    w, h = 230 * size, 40 * size
    op({"op": "select", "mode": "ellipse", "region": [x - 60 * size, base + 2, w, h], "antialias": True})
    op({"op": "select.feather", "radius": 14})
    op({"op": "fill", "layer": "그림자", "color": [25, 60, 30, 150]})
op({"op": "select"})
op({"op": "layer.setProps", "layer": "그림자", "blendMode": "multiply", "opacity": 0.75})

# 꽃(길 근처 촘촘히)
cur[0] = "풀"
random.seed(99)
for i in range(260):
    x = random.uniform(0, W); y = random.uniform(760, H)
    if y < hill_y(x, 655, 35, 0.4) + 10: continue
    d = random.uniform(3, 7) * (0.5 + (y - 700) / 300)
    col = random.choice(["#FFE45A", "#FF8FB3", "#FFFFFF", "#FFB84D", "#C9A3FF", "#FF6B6B"])
    dab("납작붓", d, col, x, y)
    dab("납작붓", d * 0.45, "#FFF7C2", x - d * 0.15, y - d * 0.15)

# 새
op({"op": "layer.add", "name": "새"})
for (bx, by, sc) in [(700, 420, 1.0), (750, 445, 0.8), (800, 425, 0.9), (1240, 380, 0.7), (1280, 400, 0.55)]:
    stroke("잉크펜", 2.4 * sc, "#2B3A4A", [(bx - 16 * sc, by - 2 * sc), (bx - 7 * sc, by + 6 * sc), (bx, by), (bx + 7 * sc, by + 6 * sc), (bx + 16 * sc, by - 2 * sc)], profile="taper-in-out")

# 마무리: 색 보정(살짝 대비 + 따뜻하게), 비네팅
op({"op": "layer.add", "name": "비네팅"})
op({"op": "select", "mode": "ellipse", "region": [-200, -200, W + 400, H + 400], "antialias": True})
op({"op": "select.invert"})
op({"op": "select.feather", "radius": 200})
op({"op": "fill", "layer": "비네팅", "color": [20, 30, 50, 120]})
op({"op": "select"})
op({"op": "layer.setProps", "layer": "비네팅", "blendMode": "multiply", "opacity": 0.35})

op({"op": "render", "view": {"mode": "full", "max": 1600}})
op({"op": "doc.save", "path": "out/meadow.ora"})
op({"op": "doc.save", "path": "out/meadow.psd"})
json.dump(ops, open(r"out/meadow.json", "w", encoding="utf-8"), ensure_ascii=False)
print(len(ops), "ops")
