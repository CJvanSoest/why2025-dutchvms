#!/usr/bin/env python3
"""Generate the DutchVMS logo as an SVG, geometrically matching the real
boot-splash windmill in why2025-apps/apps/cj_launcher/main.c
(draw_splash_view() / draw_sail() / NEON_ORANGE / NEON_DIM), captured at the
same "X-shape" starting rotation the splash animation itself starts from
(angle_idx = 2, 6, 10, 14 out of a 16-step table -- i.e. 45 degrees apart).

Not a copy-paste of the C drawing code (that draws into a raw framebuffer
frame by frame); this recomputes the same trapezoid/line geometry directly
as SVG path/line elements for a single static frame, since a README image
needs one frame, not an animation loop.

Run from this directory:

    python3 generate_logo.py

Writes dutchvms-logo.svg next to this script.
"""
import math
import os

OUT = os.path.dirname(os.path.abspath(__file__))

# Same palette as cj_launcher's splash screen (main.c NEON_ORANGE/NEON_DIM)
# and the Tokyo-Night-esque palette used across this fork's other apps.
BG = "#000000"
NEON_ORANGE = "#FF6B1A"
NEON_DIM = "#803608"
STAR_DIM = "#3A3A46"

W, H = 480, 560
CX = W // 2
HUB_Y = 260
SAIL_LEN = 130
SAIL_W_HALF = 17
STROKE = 3


def sail_points(hub_x, hub_y, length, half_w, angle_deg):
    """Mirrors draw_sail(): a 3-sided open rectangle (hub side open) plus a
    center spine and 6 lattice rungs, for one sail at angle_deg."""
    a = math.radians(angle_deg)
    axis_dx, axis_dy = math.cos(a), -math.sin(a)  # screen Y points down
    perp_dx, perp_dy = math.sin(a), math.cos(a)

    tip_x = hub_x + axis_dx * length
    tip_y = hub_y + axis_dy * length
    dpx, dpy = perp_dx * half_w, perp_dy * half_w

    lines = [
        ((hub_x + dpx, hub_y + dpy), (tip_x + dpx, tip_y + dpy)),
        ((hub_x - dpx, hub_y - dpy), (tip_x - dpx, tip_y - dpy)),
        ((tip_x + dpx, tip_y + dpy), (tip_x - dpx, tip_y - dpy)),
        ((hub_x, hub_y), (tip_x, tip_y)),
    ]
    for r in range(1, 7):
        d = length * r / 7
        rx, ry = hub_x + axis_dx * d, hub_y + axis_dy * d
        lines.append(((rx + dpx, ry + dpy), (rx - dpx, ry - dpy)))
    return lines


def line(p1, p2, color=NEON_ORANGE, width=STROKE):
    return (
        f'<line x1="{p1[0]:.1f}" y1="{p1[1]:.1f}" x2="{p2[0]:.1f}" y2="{p2[1]:.1f}" '
        f'stroke="{color}" stroke-width="{width}" stroke-linecap="round"/>'
    )


def build_svg():
    parts = []
    parts.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
        f'width="{W}" height="{H}" role="img" aria-label="DutchVMS logo">'
    )
    parts.append(f'<rect width="{W}" height="{H}" fill="{BG}"/>')

    # Sparse starfield background, deterministic (no live randomness in a
    # generator script that must reproduce byte-identical output on rerun).
    star_seed = [
        (37, 41), (92, 88), (150, 30), (210, 70), (300, 40), (360, 95),
        (420, 130), (60, 180), (440, 220), (410, 300), (30, 340), (60, 480),
        (420, 460), (450, 400), (20, 420), (250, 20), (180, 500), (330, 510),
    ]
    for sx, sy in star_seed:
        parts.append(f'<circle cx="{sx}" cy="{sy}" r="1.4" fill="{STAR_DIM}"/>')

    # ---- Sails (X-shape start: angle_idx 2, 6, 10, 14 of 16 -> 45 deg apart,
    # offset 45 deg from vertical) ----
    for angle_idx in (2, 6, 10, 14):
        angle_deg = angle_idx * 22.5
        for p1, p2 in sail_points(CX, HUB_Y, SAIL_LEN, SAIL_W_HALF, angle_deg):
            parts.append(line(p1, p2))

    # Hub
    parts.append(f'<circle cx="{CX}" cy="{HUB_Y}" r="10" fill="none" stroke="{NEON_ORANGE}" stroke-width="3"/>')
    parts.append(f'<rect x="{CX - 3}" y="{HUB_Y - 3}" width="6" height="6" fill="{NEON_ORANGE}"/>')

    # ---- Cap ----
    cap_top_y = HUB_Y + 15
    cap_h, cap_w = 25, 42
    parts.append(line((CX - cap_w / 2 + 7, cap_top_y), (CX + cap_w / 2 - 7, cap_top_y)))
    parts.append(line((CX - cap_w / 2, cap_top_y + 7), (CX - cap_w / 2 + 7, cap_top_y)))
    parts.append(line((CX + cap_w / 2, cap_top_y + 7), (CX + cap_w / 2 - 7, cap_top_y)))
    parts.append(line((CX - cap_w / 2, cap_top_y + 7), (CX - cap_w / 2, cap_top_y + cap_h)))
    parts.append(line((CX + cap_w / 2, cap_top_y + 7), (CX + cap_w / 2, cap_top_y + cap_h)))

    # ---- Upper body ----
    upper_top_y = cap_top_y + cap_h + 2
    upper_h = 63
    upper_top_half_w, upper_bot_half_w = 38, 49
    parts.append(line((CX - cap_w / 2, cap_top_y + cap_h), (CX - upper_top_half_w, upper_top_y)))
    parts.append(line((CX + cap_w / 2, cap_top_y + cap_h), (CX + upper_top_half_w, upper_top_y)))
    parts.append(line((CX - upper_top_half_w, upper_top_y), (CX + upper_top_half_w, upper_top_y)))
    parts.append(line((CX - upper_top_half_w, upper_top_y), (CX - upper_bot_half_w, upper_top_y + upper_h)))
    parts.append(line((CX + upper_top_half_w, upper_top_y), (CX + upper_bot_half_w, upper_top_y + upper_h)))
    parts.append(
        f'<rect x="{CX - 7}" y="{upper_top_y + 12:.1f}" width="14" height="18" fill="none" '
        f'stroke="{NEON_ORANGE}" stroke-width="2"/>'
    )

    # ---- Balcony ----
    balcony_y = upper_top_y + upper_h
    balcony_half = upper_bot_half_w + 15
    parts.append(line((CX - balcony_half, balcony_y), (CX + balcony_half, balcony_y), width=4))
    parts.append(line((CX - balcony_half, balcony_y + 7), (CX + balcony_half, balcony_y + 7)))
    rx = CX - balcony_half + 5
    while rx <= CX + balcony_half - 5:
        parts.append(line((rx, balcony_y - 7), (rx, balcony_y), width=1.5))
        rx += 10

    # ---- Lower body ----
    lower_top_y = balcony_y + 12
    lower_h = 108
    lower_top_half = upper_bot_half_w + 2
    lower_bot_half = upper_bot_half_w + 15
    parts.append(line((CX - lower_top_half, lower_top_y), (CX - lower_bot_half, lower_top_y + lower_h)))
    parts.append(line((CX + lower_top_half, lower_top_y), (CX + lower_bot_half, lower_top_y + lower_h)))
    parts.append(
        line(
            (CX - lower_bot_half - 18, lower_top_y + lower_h),
            (CX + lower_bot_half + 18, lower_top_y + lower_h),
            width=4,
        )
    )
    parts.append(
        f'<rect x="{CX - 33}" y="{lower_top_y + 21:.1f}" width="14" height="18" fill="none" '
        f'stroke="{NEON_ORANGE}" stroke-width="2"/>'
    )
    parts.append(
        f'<rect x="{CX + 19}" y="{lower_top_y + 21:.1f}" width="14" height="18" fill="none" '
        f'stroke="{NEON_ORANGE}" stroke-width="2"/>'
    )
    door_y = lower_top_y + lower_h - 42
    parts.append(
        f'<rect x="{CX - 11}" y="{door_y:.1f}" width="22" height="42" fill="none" '
        f'stroke="{NEON_ORANGE}" stroke-width="2"/>'
    )
    parts.append(line((CX, door_y + 4), (CX, lower_top_y + lower_h - 2), width=1))

    # ---- Wordmark ----
    text_y = H - 40
    parts.append(
        f'<text x="{CX}" y="{text_y}" text-anchor="middle" '
        f'font-family="\'Courier New\', ui-monospace, monospace" font-weight="bold" '
        f'font-size="34" fill="{NEON_ORANGE}" letter-spacing="2">DutchVMS</text>'
    )
    parts.append(
        f'<text x="{CX}" y="{text_y + 24}" text-anchor="middle" '
        f'font-family="\'Courier New\', ui-monospace, monospace" '
        f'font-size="13" fill="#A0A0B0" letter-spacing="1">WHY2025 badge firmware</text>'
    )

    parts.append("</svg>")
    return "\n".join(parts)


if __name__ == "__main__":
    svg = build_svg()
    out_path = os.path.join(OUT, "dutchvms-logo.svg")
    with open(out_path, "w") as f:
        f.write(svg)
        f.write("\n")
    print(f"wrote {out_path}")
