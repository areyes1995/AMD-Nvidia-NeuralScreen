# Solicitud de código fuente HIP (GPL-3.0) — `dlssnr_amd_pass0/1.dll`

> Estado: **LISTO PARA ENVIAR EN UN CLIC**.
> `python tools/send_gpl_request.py` abre el issue ya rellenado en
> github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/new; sólo hay
> que pulsar «Submit new issue» con tu cuenta. Nada se publica solo.
>
> Destino verificado (2026-09-16): el repo existe, es GPL-3.0 y tiene los
> issues habilitados (39 abiertos).

## Cómo enviarlo

| Vía | Comando | Notas |
| --- | --- | --- |
| Navegador (recomendado) | `python tools/send_gpl_request.py` | Abre el formulario con título y cuerpo puestos. Publicas tú. |
| Sólo la URL | `python tools/send_gpl_request.py --print` | Para pegar el enlace en otro navegador/máquina. |
| `gh` CLI | `python tools/send_gpl_request.py --gh` | Requiere `gh auth login`. Pide confirmación antes de crear el issue. |
| Manual | copia los bloques de abajo | Título y cuerpo son los mismos que usa el script. |

`python` = `%LOCALAPPDATA%\Programs\Python\Python313\python.exe` en esta
máquina (el `python` del PATH es el stub de la Store).

El script **parsea este documento**: los dos bloques marcados abajo son la
única fuente de verdad. Si editas el texto, el issue cambia con él.

## Issue title

<!-- ns:title -->
```text
GPL-3.0 source request: HIP neural-rendering backend (dlssnr_amd_pass0/1.dll)
```

## Issue body (EN — canónico, lo parsea `tools/send_gpl_request.py`)

<!-- ns:body -->
````text
Hi — this is a GPL-3.0 §6 request for the corresponding source of the HIP
neural-rendering backend shipped in the AMD PreSR Multipass pack
(`OptiScaler-AMD-PreSR-Multipass-v1.7.3`).

### What this is about

The pack ships `dlssnr_amd_pass0.dll` / `dlssnr_amd_pass1.dll` and
`dlssnr_on_amd_weights.bin` as binaries only. This project is GPL-3.0 and
those DLLs implement the DlssNr multipass pipeline documented here, so as a
recipient of the binaries I'm asking for the complete corresponding source
under §6 — the machine-readable sources plus the scripts and instructions
needed to rebuild and install them.

### What I'm asking for

1. **HIP kernel sources** (`.hip` / `.cpp` / `.hlsl`) implementing the
   DLSS-NR multipass network.
2. **The graph definition**: layer order, tensor shapes and dtypes,
   activations, and how the 153 weight blobs map onto passes 0-2
   (including the pre-SR ping-pong and the residual composition).
3. **The weight converter/reader** used to produce
   `dlssnr_on_amd_weights.bin`.
4. **The host glue**: `hipImportExternalMemory` interop, semaphore
   sync, D3D12 <-> HIP resource sharing, and the `DlssNr_Dx12::Dispatch`
   entry point (params `DLSSNR.Color/Output/MVec/Depth/DepthInverted`,
   subrects, `Intensity` / `LocalToneStrength` /
   `LocalStructureStrength` / `SkinStructureStrength` / `UseAutoMask` /
   `Style` / `UICorrection`).
5. **Build instructions**: HIP SDK and clang version (the blobs report
   clang 21.0.0) and the `gfx1100/1101/1102/1200/1201` targeting flags.

### What I already reconstructed, so you know the exact gap

I decoded the weights container byte-for-byte against the real 140.8 MB
file:

```
offset 0   magic "DLSSNRW1" (8 bytes)
offset 8   u32 LE count = 153
offset 12  u32 LE data_start (absolute offset of the tensor data)
offset 16  153 x { u8 len, name[len], u64 data_off, u64 size }
           data_off relative to data_start, entries contiguous,
           name table + data == file size exactly
```

Names are blocks 0..70 (`blockN.layerM.layer`) plus one fp16 scalar,
`block70.layer0.blend_scale`. On the runtime side `amdhip64_7.dll` loads
and enumerates the device fine on my card.

What the file does **not** carry — and what items 1-2 above are really
about — is shapes, dtypes, layer order, activations and the pass
composition. Without those the weights are unusable; my AMD worker runs
the full pipeline today (capture, motion, exposure, resets, tuning) with a
passthrough dispatch where the network should be.

### Context

I'm building a GPL-3.0 desktop app (NeuralScreen, AMD backend) and you
already granted me permission to reuse the OptiScaler-DLSSNR design; the
derived code is quarantined under `amd_mode/third_party/` with GPL headers
and full attribution in `THIRD_PARTY_NOTICES.md`. A GPL-3.0 release of the
HIP sources is therefore fully compatible with what I'm doing.

If publishing a full repo isn't practical right now, a tarball through any
channel you prefer satisfies §6 just as well. I have an RX 9070 XT here, so
I'm happy to test the rebuild, send back build fixes and report results.

Thanks for the pack and for the work behind it.
````

## Texto corto alternativo (ES, por DM/email — no lo usa el script)

```text
Hola — escribo por el backend HIP del pack OptiScaler-AMD-PreSR-Multipass
v1.7.3. Como los .dll derivan de código GPL-3.0 y se distribuyen sin fuente,
pido el "corresponding source" (GPL-3.0 §6): kernels HIP, definición del
grafo (orden, shapes, dtypes, activaciones, mapeo de los 153 tensores a los
pases 0-2), lector/conversor del .bin, glue HIP<->D3D12 (ExternalMemory +
semáforos, DlssNr_Dx12::Dispatch) e instrucciones de build (HIP SDK/clang,
targets gfx1100/1101/1102/1200/1201). Tengo tu permiso para reutilizar el
diseño en un proyecto GPL-3.0 y puedo validar el rebuild en una RX 9070 XT.
Gracias.
```

## Corrección importante (2026-09-16)

El backend HIP no es de wilsjo2: el binario se identifica como
**"Danielblnc's DLSS-NR on AMD"** (v0.2.14 en el pack, v0.2.18 la build
standalone). wilsjo2 lo redistribuye dentro del pack. Antes de enviar,
valora: el issue sigue siendo válido contra quien distribuye (GPL-3.0 §6
obliga al distribuidor), pero el fuente lo tiene Danielblnc — conviene
pedírselo también a él. Ver `docs/AMD_HIP_HOSTING.md`.

## Por qué este pedido tiene fuerza

- No es un favor: la GPL-3.0 §6 **obliga** a entregar el medio de
  reconstruir los binarios a quien los recibe.
- Ya tenemos permiso del dueño del repo upstream (2026-09-16), así que
  publicar la fuente bajo GPL-3.0 no crea conflicto con nuestro proyecto.
- Sin esta fuente, la Fase 2b (dispatch neuronal real) no puede empezar;
  el plan B es reconstruir el grafo a mano sobre ONNX + DirectML (semanas,
  con riesgo de no clavar el multipass exacto).

## Si no hay respuesta

1. Esperar ~7 días y hacer un bump educado en el mismo issue.
2. En paralelo, arrancar el plan B (ONNX + DirectML, `amd_mode/python/executor.py`
   ya tiene el probe listo) para no dejar la 2b parada.
3. Última instancia: reportar la distribución sin fuente a los upstreams
   GPL afectados (OptiScaler). Sólo si el autor se niega explícitamente.
