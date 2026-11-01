# EasyController wheel art

The character swap wheel (`Addons/EsteriaSwap`) uses the radial-wheel art of
[Easy Controller - Forever](https://github.com/moust4ki/EasyControllerWowForever) 1.19.2 by moust4ki, under the MIT
license in `LICENSE`.

- `wheel_bg_N.png`: the wheel cut into N sections (1 to 8), from `design/radial/wheel/`.
- `wheel_sel_N.png`, `wheel_off_N.png`: the highlight and the veil of section 1 (12 o'clock) on the same 512 canvas.
  The addon rotates them about the wheel's centre onto the other sections.
- `ck_slot.png`, `ck_slot_glow.png`, `ck_hub.png`: the round slot, its hover ring and the hub, from `textures/`.

`Tools/EsteriaShell/build_shell_mpq.py` converts them to TGA under `Interface\AddOns\EsteriaSwap\Textures\`.
