"""The NVIDIA backend's NR parameter vocabulary.

Profiles, slider ranges and clamping for the NGX neural pass (feature 18).
Dependency-free on purpose: settings_io re-exports these names, and the
AMD backend (amd_mode/python/params.py) mirrors this shape so the menu
and the pipeline can treat both backends the same.
"""
from __future__ import annotations


# --- DLSS 5 NR profiles (field order as in the converter) -----------------
#
# local_tone is half a point lower in every profile than it was through
# 1.8.1 (user, 13.09). The local tone mapping is the part that lifts
# shadows and flattens contrast, and at the old values it was doing more of
# that than the picture wanted - most visibly on dark scenes, where the
# brightening this program does anyway meets it head on. The four sliders
# still reach everything they reached: this moves where the profiles sit,
# not what the range allows.
#
# The profiles no longer carry `profile`, `preset` or `ui_correction`.
# Measured on the 310.8.0 runtime: every value of all three produces a
# byte-identical frame (colour-sweep-20260913, and tests/test_param_effect.py
# reports them every run). They are still sent - the wire layout is shared
# with the resize command and with a hundred tests - but they are sent as a
# fixed zero by the two packers, and nobody has to wonder about them again.
#
# Intensity is clamped at 1.00 inside NVIDIA's DLL, so the 1.65 and 2.50 the
# strong profiles used to ask for were the same picture as 1.00 all along.
# Extreme's local_structure comes down from 2.00 to the new 1.50 ceiling,
# which is the one real change here: measured, that is a detail metric of
# -14.4% against -13.5%, about a percent of the picture.
#
# The auto mask is on in every profile. Measured here on three real frames,
# a frozen input so anything moving between consecutive outputs is the
# network trembling rather than the picture changing:
#
#   source  mask   wiggle  shadows  peak   edges  detail  ms/frame
#   text       0    0.209    0.167    10   1.110   0.965      5.26
#   text       1    0.158    0.131    10   1.096   0.932      5.15
#   film       0    0.363    0.207     9   0.971   0.666      5.26
#   film       1    0.314    0.176     7   0.959   0.669      5.30
#   game       0    0.335    0.304     8   1.225   1.176      5.26
#   game       1    0.308    0.286     6   1.213   1.157      5.26
#
# It damps the trembling by 8-24% and the shadows by 6-22%, takes the peaks
# down (9->7, 8->6), and costs nothing in time - the per-frame figures are
# the same within noise. It is not free: on text it costs 3.4% of the fine
# detail. Text is also where it damps the most, and shimmering text is what
# people report, so that is the trade taken.
#
# The other reason is arithmetic: skin_structure is inert without it. With
# the mask off in two of the four profiles, the fourth slider in the menu
# did nothing at all in those two.
PROFILES = {
    "Faithful": dict(style=0, auto_mask=1,
                     intensity=0.70, local_tone=0.25, local_structure=0.75, skin_structure=-1.0),
    "Natural": dict(style=1, auto_mask=1,
                    intensity=1.00, local_tone=0.50, local_structure=1.00, skin_structure=-1.0),
    "Strong / Cinematic": dict(style=2, auto_mask=1,
                               intensity=1.00, local_tone=0.90, local_structure=1.50, skin_structure=1.0),
    "Extreme / Overdrive": dict(style=2, auto_mask=1,
                                intensity=1.00, local_tone=1.50, local_structure=1.50, skin_structure=1.5),
}


# How far each slider really reaches. One shared 0..2.5 was wrong in both
# directions: it promised travel that did nothing (issue #40, "low effect
# strength" - the user was turning a knob that had stopped answering), and
# it allowed values where the picture gets worse rather than stronger.
#
#   intensity        clamped at 1.0 inside NVIDIA's DLL. 1.0, 1.25, 1.5, 2
#                    and 2.5 all hash to the same frame (re-measured 15.09
#                    on 310.8.0 and again with the +0.5 tops request - still
#                    dead). The slider STAYS at 1.0: a longer travel would be
#                    the exact lie the range was rebuilt to remove.
#   tone, structure  the detail metric keeps climbing past 1.5, and so does
#                    the shimmer: the metric counts trembling noise as fine
#                    detail. Measured 15.09: 2.0 still moves the picture
#                    (distinct frames) and is a stronger look, so the top
#                    moved 1.5 -> 2.0 on request; shimmer at 2.0/2.0 is
#                    ~2.1 of 255 against ~1.5 at the old tops (test_param_effect
#                    pins the ceilings).
#   skin_structure   inert unless auto_mask is on, and -1 is "off".
#                    2.5 measured alive on 15.09; the top moved 2.0 -> 2.5.
PARAM_RANGE = {
    "intensity": (0.0, 1.0),
    "local_tone": (0.0, 2.0),
    "local_structure": (0.0, 2.0),
    "skin_structure": (-1.0, 2.5),
}


def param_range(key: str) -> tuple:
    """The (low, high) a parameter is allowed. Unknown keys get the widest."""
    return PARAM_RANGE.get(key, (0.0, 1.5))


def clamp_param(key: str, value: float) -> float:
    """Pull a value into range - for configs written before the range was."""
    lo, hi = param_range(key)
    return min(max(float(value), lo), hi)


# The four sliders a user preset stores. The same keys as PROFILES carries,
# minus the NGX plumbing (profile/preset/style/auto_mask/ui_correction stay
# tied to the built-in profile the preset was saved from).
PRESET_KEYS = ("intensity", "local_tone", "local_structure", "skin_structure")


# The NGX plumbing a preset carries along with the four sliders: the range
# it must be in, and what to use when it is not there at all. Presets saved
# by builds up to 1.8.2 also carry profile/preset/ui_correction; those are
# read and thrown away, because they do nothing (see PARAM_RANGE). A preset
# saved by this build does not have them, and must still load.
_PRESET_INT_KEYS = {
    "style": (0, 2, 1),
    "auto_mask": (0, 1, 0),
}
