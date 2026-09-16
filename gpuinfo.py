"""GPU model and architecture, through nvapi, with no external processes.

The interface needs it: to show what we are running on and whether Neural
Rendering is available. The architecture is read the same way NVIDIA's own
library reads it (nvapi_QueryInterface -> NvAPI_GPU_GetArchInfo), so the
value matches the one it makes its decision on.

Everything is wrapped in try: without nvapi (a non-NVIDIA machine, a
stripped driver) the module returns empty fields instead of taking the
program down.
"""
from __future__ import annotations

import ctypes

# nvapi function ids are hashes of their names
_ID_INITIALIZE = 0x0150E828
_ID_ENUM_GPUS = 0xE5AC921F
_ID_GET_ARCH = 0xD8265D24
_ID_GET_NAME = 0xCEEE8E9F

# NV_GPU_ARCHITECTURE_ID: the group lives in the high bits. Neural Rendering
# (feature 18) officially requires Blackwell — see NGXGpuArchitecture inside
# nvngx_dlssnr.dll itself. Verified against NVIDIA's nvapi.h (TU100=0x160,
# GA100=0x170, AD100=0x190, GB200=0x1B0) and open-gpu-kernel-modules
# nv_arch.h (Turing=0x160, Ampere=0x170, Hopper=0x180, Ada=0x190,
# Blackwell GB1XX=0x1A0, GB2XX=0x1B0). Real-user logs confirm: RTX 2070
# reports 0x160, RTX 3060 Ti reports 0x170.
ARCH_NAMES = {
    0x160: ("Turing", "20xx"),
    0x170: ("Ampere", "30xx"),
    0x180: ("Hopper", ""),
    0x190: ("Ada", "40xx"),
    0x1A0: ("Blackwell", "50xx"),
    0x1B0: ("Blackwell", "50xx"),
    0x1C0: ("Blackwell", "50xx"),
}
ARCH_BLACKWELL = 0x1A0


class _ArchInfo(ctypes.Structure):
    _fields_ = [("version", ctypes.c_uint32),
                ("architecture", ctypes.c_uint32),
                ("implementation", ctypes.c_uint32),
                ("revision", ctypes.c_uint32)]


def _choose_index(names: list, hint) -> int:
    """Which card the hint names, or 0 - the first one.

    The hint is the DXGI name of the adapter the pipeline will really run
    on. NVAPI enumerates in its own order and the two disagree on
    multi-GPU machines - the first NVAPI card can be the one that does no
    work at all (issue #81: the menu said RTX 3050 while the 4070 Super
    did everything). No hint, no cards, or nothing matching keeps the
    first card, which is what a single-card machine wants.
    """
    if hint:
        wanted = str(hint).strip().casefold()
        for i, nm in enumerate(names):
            if str(nm).strip().casefold() == wanted:
                return i
    return 0


def probe(name_hint: str | None = None) -> dict:
    """{name, arch, arch_group, family, official} — empty fields on failure.

    name_hint says WHICH card to describe, by its DXGI name: the caller
    passes the adapter the worker will really run on, so the line on
    screen and the log name the card that does the work (issue #81).
    Without a hint the first card is described.
    """
    out = {"name": "", "arch": "", "arch_group": 0, "family": "",
           "official": False}
    try:
        nvapi = ctypes.WinDLL("nvapi64.dll")
        qi = nvapi.nvapi_QueryInterface
        qi.restype = ctypes.c_void_p
        qi.argtypes = [ctypes.c_uint32]

        p_init, p_enum = qi(_ID_INITIALIZE), qi(_ID_ENUM_GPUS)
        if not p_init or not p_enum:
            return out
        if ctypes.CFUNCTYPE(ctypes.c_int)(p_init)() != 0:
            return out

        handles = (ctypes.c_void_p * 64)()
        count = ctypes.c_uint32(0)
        enum = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_void_p),
                                ctypes.POINTER(ctypes.c_uint32))(p_enum)
        if enum(handles, ctypes.byref(count)) != 0 or count.value == 0:
            return out

        p_name = qi(_ID_GET_NAME)
        # Every card's name first: the hint can only be matched against a
        # name, and reading the names is the only way to know which handle
        # is which - NVAPI's order is not DXGI's (see _choose_index).
        names = []
        name_fn = (ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                    ctypes.c_char_p)(p_name)
                   if p_name else None)
        for h in handles[:count.value]:
            nm = ""
            if name_fn is not None:
                buf = ctypes.create_string_buffer(64)
                if name_fn(h, buf) == 0:
                    nm = buf.value.decode("ascii", "replace").strip()
            names.append(nm)
        chosen = _choose_index(names, name_hint)
        gpu = handles[chosen]

        name = names[chosen]
        if name:
            # "NVIDIA GeForce RTX 5070 Ti" -> "RTX 5070 Ti": the full name
            # does not fit the menu line, and the vendor adds nothing there.
            for prefix in ("NVIDIA GeForce ", "NVIDIA "):
                if name.startswith(prefix):
                    name = name[len(prefix):]
                    break
            out["name"] = name

        p_arch = qi(_ID_GET_ARCH)
        if p_arch:
            fn = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                  ctypes.POINTER(_ArchInfo))(p_arch)
            for ver in (2, 1):
                info = _ArchInfo()
                info.version = ctypes.sizeof(_ArchInfo) | (ver << 16)
                if fn(gpu, ctypes.byref(info)) == 0:
                    group = info.architecture & 0xFFFFFFF0
                    arch, family = ARCH_NAMES.get(group, ("", ""))
                    out["arch_group"] = group
                    out["arch"] = arch
                    out["family"] = family
                    out["official"] = group >= ARCH_BLACKWELL
                    break
    except Exception:
        return out
    return out


def describe(info: dict) -> str:
    """Menu line: "RTX 5070 Ti · Blackwell". Empty when the GPU is unknown —
    the menu shows its own placeholder rather than an English string in a
    localised interface."""
    name = info.get("name")
    if not name:
        return ""
    arch = info.get("arch")
    return f"{name} · {arch}" if arch else name


#: PCI vendor IDs, as DXGI reports them in AdapterDesc.VendorId.
VENDOR_IDS = {
    0x10DE: "NVIDIA",
    0x1002: "AMD",
    0x8086: "Intel",
    0x1414: "Microsoft",
}

#: Prefixes that add nothing to a menu line.
_VENDOR_PREFIXES = ("NVIDIA GeForce ", "NVIDIA ", "AMD Radeon ",
                    "Intel(R) Arc(TM) ", "Intel(R) ")


def system_adapters() -> list:
    """Every physical GPU as [(dxgi_index, vendor, name), ...].

    Unlike capture.list_adapters (NVIDIA only, for the worker picker),
    this lists all vendors: the program needs to know whether the
    machine can run the neural pass at all (NVIDIA) or stays degraded
    (AMD/Intel). Empty on failure - never raises.
    """
    try:
        from dxcam.core.device import Device
        from dxcam.util.io import enum_dxgi_adapters
    except Exception:
        return []
    out = []
    try:
        for idx, adapter in enumerate(enum_dxgi_adapters()):
            try:
                desc = Device(adapter).desc
            except Exception:
                continue
            if getattr(desc, "Flags", 0) & 2:  # DXGI_ADAPTER_FLAG_SOFTWARE
                continue
            try:
                vendor = VENDOR_IDS.get(int(getattr(desc, "VendorId", 0)),
                                        "Unknown")
            except (TypeError, ValueError):
                vendor = "Unknown"
            out.append((idx, vendor, str(desc.Description).strip()))
    except Exception:
        return []
    return out


def short_name(vendor: str, name: str) -> str:
    """Card name without the vendor boilerplate ("RTX 4070")."""
    for prefix in _VENDOR_PREFIXES:
        if name.startswith(prefix):
            return name[len(prefix):].strip()
    return name.strip()


def primary_gpu(adapters=None) -> dict:
    """{"vendor", "name"} of the card that matters: NVIDIA first.

    A hybrid laptop lists the integrated GPU next to the discrete one -
    the neural pass can only ever run on NVIDIA, so that one wins even
    when it is not first in DXGI order. {"vendor": "Unknown", "name": ""}
    when nothing is found.
    """
    if adapters is None:
        adapters = system_adapters()
    if not adapters:
        return {"vendor": "Unknown", "name": ""}
    for _idx, vendor, name in adapters:
        if vendor == "NVIDIA":
            return {"vendor": vendor, "name": short_name(vendor, name)}
    _idx, vendor, name = adapters[0]
    return {"vendor": vendor, "name": short_name(vendor, name)}


def has_nvidia(adapters=None) -> bool:
    """Whether any physical GPU can run the neural pass (NVIDIA)."""
    if adapters is None:
        adapters = system_adapters()
    return any(vendor == "NVIDIA" for _idx, vendor, _name in adapters)


def system_using_label(info=None) -> str:
    """Menu label: "System using: NVIDIA RTX 4070" / "...: AMD ...".

    The vendor word is what gates the features; the card name is what
    tells the user which one was picked on multi-GPU machines.
    """
    if info is None:
        info = primary_gpu()
    vendor = str(info.get("vendor") or "Unknown")
    name = str(info.get("name") or "").strip()
    return f"System using: {vendor} {name}".strip()


if __name__ == "__main__":
    got = probe()
    print(describe(got) or "unknown GPU")
    print(f"group 0x{got['arch_group']:X}, officially supported: "
          f"{'yes' if got['official'] else 'no'}")
