# Ground truth for the frame-generation benchmark: a two-layer panorama (far scene and a near
# layer with more parallax) seen by a camera that pans slowly, fast, flicks and strafes while
# aiming, with a static HUD on top. Deterministic: render(i) is the 120 fps picture i.
import json
import math
import cv2
import numpy as np

W, H = 1920, 1080
FPS = 120
SECONDS = 12.0
FRAMES = int(FPS * SECONDS)
PANO_W, PANO_H = 8192, 1600
NEAR_PARALLAX = 1.7

# (name, start s, end s): what the measure script reports per segment.
SEGMENTS = [("still", 0.0, 0.5), ("slow", 0.5, 3.0), ("fast", 3.0, 5.5), ("flick", 5.5, 8.0),
            ("aim", 8.0, 10.5), ("veryfast", 10.5, 12.0)]

CROSSHAIR_BOX = (W // 2 - 48, H // 2 - 48, 96, 96)          # x, y, w, h
HUD_BOXES = {"hud_tl": (24, 20, 420, 110), "hud_tr": (1650, 20, 250, 64), "minimap": (1660, 820, 220, 220)}


def velocity(t):
    """Camera velocity in panorama pixels per 120 fps picture."""
    if t < 0.5:
        return 0.0, 0.0
    if t < 3.0:
        return 1.5, 0.3 * math.sin(t * 2.0)
    if t < 5.5:
        return -10.0, 1.0 * math.sin(t * 3.0)
    if t < 8.0:
        phase = (t - 5.5) % 0.5
        k = int((t - 5.5) / 0.5)
        if phase < 0.12:
            return (45.0 if k % 2 == 0 else -40.0), (6.0 if k % 3 == 0 else -4.0)
        return 0.6, 0.0
    if t < 10.5:
        return 3.0, 0.2 * math.sin(t * 5.0)
    return 20.0, -2.0


def crosshair_alpha(t):
    """Aiming down sights: the crosshair fades in and out during the aim segment."""
    if not 8.0 <= t < 10.5:
        return 1.0
    c = (t - 8.0) % 1.2
    if c < 0.1:
        return c / 0.1
    if c < 0.7:
        return 1.0
    if c < 0.8:
        return 1.0 - (c - 0.7) / 0.1
    return 0.0


def camera_path():
    xs, ys = np.zeros(FRAMES), np.zeros(FRAMES)
    x, y = 3000.0, 260.0
    for i in range(FRAMES):
        xs[i], ys[i] = x, y
        vx, vy = velocity(i / FPS)
        x += vx
        y = min(max(y + vy, 40.0), PANO_H - H - 40.0)
    return xs, ys


def _noise(rng, w, h, octaves):
    acc = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    for o in octaves:
        g = rng.random((max(2, h // o), max(2, w // o))).astype(np.float32)
        acc += amp * cv2.resize(g, (w, h), interpolation=cv2.INTER_CUBIC)
        total += amp
        amp *= 0.55
    return acc / total


def _wrap_rect(img, x, y, w, h, color):
    for dx in (0, -img.shape[1]):
        cv2.rectangle(img, (x + dx, y), (x + dx + w, y + h), color, -1)


def build_layers(seed=7):
    rng = np.random.default_rng(seed)
    # Far layer: terrain-like noise, buildings with window grids, poles and wires, signs.
    n = _noise(rng, PANO_W, PANO_H, [512, 128, 32, 8, 2])
    far = np.stack([60 + 120 * n, 90 + 100 * n, 70 + 60 * (1 - n)], -1).astype(np.uint8)
    for _ in range(90):
        bw, bh = int(rng.integers(120, 420)), int(rng.integers(200, 700))
        bx, by = int(rng.integers(0, PANO_W - 1)), int(PANO_H - bh - rng.integers(0, 300))
        base = tuple(int(c) for c in rng.integers(40, 200, 3))
        _wrap_rect(far, bx, by, bw, bh, base)
        win = tuple(int(min(255, c + 70)) for c in base)
        step = int(rng.integers(14, 30))
        for wy in range(by + 10, by + bh - 10, step):
            for wx in range(bx + 8, bx + bw - 8, step):
                _wrap_rect(far, wx, wy, step // 2, step // 2, win)
    for _ in range(140):  # thin poles
        px = int(rng.integers(0, PANO_W))
        cv2.line(far, (px, 0), (px, PANO_H), tuple(int(c) for c in rng.integers(0, 255, 3)), int(rng.integers(1, 4)))
    for _ in range(30):  # wires
        py = int(rng.integers(0, PANO_H))
        cv2.line(far, (0, py), (PANO_W, py + int(rng.integers(-60, 60))), (20, 20, 20), 1, cv2.LINE_AA)
    for k in range(60):
        cv2.putText(far, f"ZONE {k:02d}", (int(rng.integers(0, PANO_W - 300)), int(rng.integers(80, PANO_H - 40))),
                    cv2.FONT_HERSHEY_DUPLEX, float(rng.uniform(1.0, 2.5)), (240, 240, 230), 2, cv2.LINE_AA)
    # Near layer (more parallax): trunks, a slatted fence and crates, with an alpha mask.
    near = np.zeros((PANO_H, PANO_W, 3), np.uint8)
    alpha = np.zeros((PANO_H, PANO_W), np.uint8)
    for _ in range(26):
        tx, tw = int(rng.integers(0, PANO_W)), int(rng.integers(30, 90))
        col = tuple(int(c) for c in rng.integers(20, 90, 3))
        _wrap_rect(near, tx, 0, tw, PANO_H, col)
        cv2.rectangle(alpha, (tx, 0), (tx + tw, PANO_H), 255, -1)
        cv2.rectangle(alpha, (tx - PANO_W, 0), (tx + tw - PANO_W, PANO_H), 255, -1)
        bark = (rng.random((PANO_H, 1)) * 40).astype(np.uint8)
        for dx in (0, -PANO_W):
            x0, x1 = max(0, tx + dx), min(PANO_W, tx + tw + dx)
            if x1 > x0:
                near[:, x0:x1] = np.clip(near[:, x0:x1].astype(np.int16) + bark[:, None, :], 0, 255).astype(np.uint8)
    fy = PANO_H - 520
    for fx in range(0, PANO_W, 22):
        cv2.rectangle(near, (fx, fy), (fx + 8, PANO_H), (150, 120, 80), -1)
        cv2.rectangle(alpha, (fx, fy), (fx + 8, PANO_H), 255, -1)
    cv2.rectangle(near, (0, fy + 30), (PANO_W, fy + 40), (130, 100, 60), -1)
    cv2.rectangle(alpha, (0, fy + 30), (PANO_W, fy + 40), 255, -1)
    return far, near, alpha


def _hud(t):
    """HUD colour and alpha planes for time t (float32, colour 0..255, alpha 0..1)."""
    col = np.zeros((H, W, 3), np.float32)
    a = np.zeros((H, W), np.float32)

    def stroke(draw, colour, alpha_value, outline=2):
        m = np.zeros((H, W), np.uint8)
        draw(m, 255, outline)
        mo = m.astype(np.float32) / 255.0 * alpha_value
        col[:] = col * (1 - mo[..., None]) + np.array((0, 0, 0), np.float32) * mo[..., None]
        a[:] = np.maximum(a, mo)
        m = np.zeros((H, W), np.uint8)
        draw(m, 255, 0)
        mi = m.astype(np.float32) / 255.0 * alpha_value
        col[:] = col * (1 - mi[..., None]) + np.array(colour, np.float32) * mi[..., None]
        a[:] = np.maximum(a, mi)

    cx, cy = W // 2, H // 2
    ca = crosshair_alpha(t)
    if ca > 0:
        def cross(m, v, grow):
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                p0 = (cx + dx * (5 - grow), cy + dy * (5 - grow))
                p1 = (cx + dx * (16 + grow), cy + dy * (16 + grow))
                cv2.line(m, p0, p1, v, 2 + 2 * grow)
            cv2.circle(m, (cx, cy), 1 + grow, v, -1)
        stroke(lambda m, v, g: cross(m, v, 1 if g else 0), (255, 255, 255), ca)
    # Thin outlined reticle ring, always there.
    stroke(lambda m, v, g: cv2.circle(m, (cx, cy), 30, v, 1 + (2 if g else 0), cv2.LINE_AA), (230, 240, 255), 0.9)

    def text(s, org, scale):
        return lambda m, v, g: cv2.putText(m, s, org, cv2.FONT_HERSHEY_SIMPLEX, scale, v, 2 + (3 if g else 0), cv2.LINE_AA)
    stroke(text("HP 100   ARMOR 75", (36, 62), 1.0), (255, 255, 255), 1.0)
    stroke(text("AMMO 30 / 120", (36, 112), 1.0), (255, 220, 120), 1.0)
    stroke(text("02:45", (1680, 68), 1.2), (255, 255, 255), 1.0)
    # Opaque minimap with a grid, a border and the player arrow.
    x0, y0, mw, mh = HUD_BOXES["minimap"]
    col[y0:y0 + mh, x0:x0 + mw] = (40, 52, 46)
    for g in range(0, mw, 22):
        col[y0:y0 + mh, x0 + g] = (80, 100, 90)
        col[y0 + g, x0:x0 + mw] = (80, 100, 90)
    cv2.rectangle(col, (x0, y0), (x0 + mw - 1, y0 + mh - 1), (220, 220, 220), 2)
    pts = np.array([[x0 + mw // 2, y0 + mh // 2 - 14], [x0 + mw // 2 - 9, y0 + mh // 2 + 10], [x0 + mw // 2 + 9, y0 + mh // 2 + 10]])
    cv2.fillPoly(col, [pts], (90, 220, 255))
    a[y0:y0 + mh, x0:x0 + mw] = 1.0
    return col, a


class Clip:
    def __init__(self):
        self.far, self.near, self.alpha = build_layers()
        self.xs, self.ys = camera_path()
        self._hud_cache = {}

    def hud(self, t):
        key = round(crosshair_alpha(t), 4)
        if key not in self._hud_cache:
            self._hud_cache[key] = _hud(t)
        return self._hud_cache[key]

    def scene(self, i):
        x, y = self.xs[i], self.ys[i]
        gx, gy = np.meshgrid(np.arange(W, dtype=np.float32), np.arange(H, dtype=np.float32))
        far = cv2.remap(self.far, gx + np.float32(x), gy + np.float32(y), cv2.INTER_CUBIC, borderMode=cv2.BORDER_WRAP)
        nx, ny = x * NEAR_PARALLAX, y * 1.15
        near = cv2.remap(self.near, gx + np.float32(nx), gy + np.float32(ny), cv2.INTER_CUBIC, borderMode=cv2.BORDER_WRAP)
        al = cv2.remap(self.alpha, gx + np.float32(nx), gy + np.float32(ny), cv2.INTER_LINEAR, borderMode=cv2.BORDER_WRAP)
        al = al.astype(np.float32)[..., None] / 255.0
        return far.astype(np.float32) * (1 - al) + near.astype(np.float32) * al

    def render(self, i):
        """BGR uint8 ground-truth picture i at 120 fps."""
        t = i / FPS
        img = self.scene(i)
        col, a = self.hud(t)
        img = img * (1 - a[..., None]) + col * a[..., None]
        return np.clip(img + 0.5, 0, 255).astype(np.uint8)


def segment_of(i):
    t = i / FPS
    for name, s, e in SEGMENTS:
        if s <= t < e:
            return name
    return SEGMENTS[-1][0]


def metadata():
    return {"width": W, "height": H, "fps": FPS, "frames": FRAMES, "segments": SEGMENTS,
            "crosshair_box": CROSSHAIR_BOX, "hud_boxes": HUD_BOXES}


if __name__ == "__main__":
    print(json.dumps(metadata(), indent=1))
