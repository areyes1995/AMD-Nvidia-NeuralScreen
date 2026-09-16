# Solicitud de código fuente HIP (GPL-3.0) — `dlssnr_amd_pass1.dll`

> Estado: BORRADOR listo para enviar. No se ha enviado nada automáticamente
> (no hay `gh` en esta máquina). Pegar como issue en el repo upstream o
> enviar por el canal de contacto del autor del pack.

## Dónde pegarlo

- Issue nuevo en https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass
  (título sugerido: `GPL source request: HIP NR backend (dlssnr_amd_pass1.dll)`)
- Referencia del pack: `OptiScaler-AMD-PreSR-Multipass-v1.7.3`
  (`dlssnr_on_amd_weights.bin` 140.8 MB, magia `DLSSNRW1`, ~153 tensores
  `blockN.layerM.layer`, DLLs `dlssnr_amd_pass0.dll` / `dlssnr_amd_pass1.dll`,
  runtime `amdhip64_7.dll`, targets `gfx1100/1101/1102/1200/1201`).

## Texto del pedido (EN, listo para pegar)

```text
Hi — GPL-3.0 corresponding-source request for the HIP neural-rendering
backend shipped in the OptiScaler AMD PreSR Multipass pack
(OptiScaler-AMD-PreSR-Multipass-v1.7.3).

The pack's `dlssnr_amd_pass1.dll` / `dlssnr_amd_pass0.dll` are derivative
works of GPL-3.0 code (this repo / OptiScaler), distributed as binaries
without the corresponding source. Under GPL-3.0 §6, please provide the
complete corresponding source needed to rebuild them, in particular:

1. The HIP kernel sources (.hip/.cpp/.hlsl) implementing the ~153-tensor
   DLSS-NR multipass network (tensors named `blockN.layerM.layer` in
   `dlssnr_on_amd_weights.bin`, magic `DLSSNRW1`).
2. The graph definition: layer order, shapes, activations, how the 153
   blobs map to passes 0-2 (incl. pre-SR + residual composition).
3. The weight-file converter/reader (header + `{u8 len, name, u64 offset,
   u64 size}` table) used to produce `dlssnr_on_amd_weights.bin`.
4. The host glue: hipImportExternalMemory interop, semaphore sync,
   D3D12 <-> HIP resource sharing, and the DlssNr_Dx12::Dispatch entry
   (params `DLSSNR.Color/Output/MVec/Depth/DepthInverted`, subrects,
   `Intensity/LocalToneStrength/LocalStructureStrength/
   SkinStructureStrength/UseAutoMask/Style/UICorrection`).
5. Build instructions (HIP SDK / clang version — blobs report clang
   21.0.0) and the gfx1100/1101/1102/1200/1201 targeting flags.

Context: I have written permission from the repo owner to reuse the
OptiScaler-DLSSNR code in a GPL-3.0 desktop project (NeuralScreen AMD
backend, quarantined under `amd_mode/third_party/`), so a GPL-3.0
release of the HIP sources is fully compatible with my use. Happy to
test the rebuild on RX 9070 XT and report back.

Thanks!
```

## Texto corto alternativo (ES, por DM/email)

```text
Hola — escribo por el backend HIP del pack OptiScaler-AMD-PreSR-Multipass
v1.7.3. Como los .dll derivan de código GPL-3.0 y se distribuyen sin fuente,
pido el "corresponding source" (GPL-3.0 §6): kernels HIP, definición del
grafo de 153 tensores, lector/conversor del .bin, glue HIP<->D3D12
(ExternalMemory + semáforos, DlssNr_Dx12::Dispatch) e instrucciones de
build. Tengo permiso del dueño del repo para reutilizar el código en un
proyecto GPL-3.0 de escritorio y puedo validar el rebuild en una RX 9070 XT.
Gracias.
```

## Por qué este pedido tiene fuerza

- No es un favor: la GPL-3.0 §6 **obliga** a entregar el medio de
  reconstruir los binarios a quien los recibe.
- Ya tenemos permiso del dueño del repo upstream, así que publicar la
  fuente bajo GPL-3.0 no crea conflicto con nuestro proyecto.
- Sin esta fuente, la Fase 2b (dispatch neuronal real) no puede empezar;
  el plan B es ONNX + DirectML con una red abierta.
