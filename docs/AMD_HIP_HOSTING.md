# El motor HIP real, hospedado dentro del worker AMD

Estado: **funcionando** (2026-09-16, RX 9070 XT / gfx1201). El pase neuronal
corre de verdad en `amd_nr_host.exe`: 6 ms/job a 320×180, 12 ms a 720p,
zero-copy, con el residual aplicado. Todo lo de aquí está verificado en esta
máquina; lo que no, se dice.

## La idea

El motor neuronal ya existe compilado: el pack de OptiScaler AMD trae
`dlssnr_amd_pass1/2/3.dll`, que son el **mismo binario** (SHA-256
`3C9CA13F…DD8`) copiado tres veces. No exporta una API — es un **proxy
`version.dll` autónomo** que, al cargarse en un proceso, engancha D3D12/DXGI
y el dispatch del upscaler FSR, y corre su red HIP sobre lo que ese proceso
renderiza.

Así que no hospedamos un SDK: **hacemos que nuestro worker parezca el juego**.
`amd_mode/native/dlssnr_engine.cpp` levanta un device D3D12, un swapchain
oculto y un dispatch FSR real; el runtime los detoura y devuelve el frame
mejorado. No hace falta el HIP SDK (los kernels van compilados dentro, sección
`.hip_fat`, 6,6 MB, targets `gfx1201,gfx1200,gfx1100,gfx1101,gfx1102`), ni
`hiprtc` — que en esta máquina no existe.

Arquitectura de la red, según sus símbolos: convoluciones fp8 (`k_conv_res`,
`k_conv_res2`, `k_conv_splitk`, `k_contract2`, `k_conv_res_views`,
`k_pre_block_1h_32_fp8`) más bloques de atención Swin/ViT-512 (`vit512a/b`,
`vit512_conv1/2`, `vit512_attn`, `swin%d_C%d`). 153 tensores.

## Instalación (BYO, nada de esto se commitea)

En `amd_mode/weights/` (o donde apunte `NS_AMD_NR_RUNTIME`):

| Archivo | Qué es |
| --- | --- |
| `version.dll` | el runtime **standalone** de Danielblnc (v0.2.18 aquí) |
| `dlssnr_on_amd_weights.bin` | los pesos (140,8 MB, magia `DLSSNRW1`) |
| `amd_fidelityfx_upscaler_dx12.dll` | el upscaler FSR del pack (FFX API 4.1.1) |
| `dlssnr_on_amd.ini` | **lo escribe el propio motor** desde el menú (`SetEffect`); esto es sólo la referencia |

```ini
[DlssNrOnAmd]
Enabled=1
UseFsrInputs=1
UseDepth=0
Interop=1
Inline=1
InlineWaitMs=200
LocalTone=0
LocalStructure=1
SkinStructure=-1
UseAutoMask=1
ToneChannels=0
Scale=0.03000
Temporal=1
Tonemap=-1
HipDevice=-1
```

Ojo con **dos builds distintas**: la del pack (v0.2.14,
`dlssnr_amd_pass*.dll`) tiene la auto-inicialización **parcheada a NOPs**
(`31 c0 90 90 90 90` justo donde iba `ff 15 …`, los seis bytes del
`call CreateThread`) porque el pack la conduce desde OptiScaler. Esa **no
sirve** aquí. La standalone (v0.2.18, la que se instala como `version.dll` en
la carpeta del juego) sí arranca sola y es la que usamos.

Sin estos archivos el worker **no falla**: sirve passthrough byte-idéntico y lo
dice en stderr. `NS_AMD_NR=0` fuerza passthrough.

## Las cuatro cosas que costó descubrir

Ninguna está documentada en ningún sitio; todas se encontraron
desensamblando el runtime y se pagan con silencio si se incumplen:

1. **El runtime tiene que entrar antes que nada D3D12.** Engancha desde un
   hilo que arranca al cargarse, y primero construye un device y un swapchain
   falsos. Un swapchain creado antes de que caiga su detour de
   `CreateSwapChain` nunca entra en su mapa *swapchain → command queue*, y su
   único plan B es `IDXGISwapChain::GetDevice(IID_ID3D12CommandQueue)`, que en
   D3D12 no puede funcionar: el frame se descarta como *"a swapchain that is
   not on our device"* y no vuelve a intentarlo. Por eso `Start()` espera a
   ver en su log la última línea de hook (`hooked IDXGISwapChain1::Present1`)
   antes de crear nada. Aquí tardan ~450 ms.
2. **`UseFsrInputs=1`.** Ese flag del ini es el que arma el hook de
   `ffxDispatch` (`GetPrivateProfileIntA("DlssNrOnAmd","UseFsrInputs",1,…)` →
   `byte_94A8E`, comprobado en el tick de cada present). Con 0, el motor se
   engancha, inicializa y **no procesa un solo frame**, sin decir nada.
3. **La red se dispara desde un dispatch del upscaler FSR**, no desde el
   present. Por eso el worker hace un `ffxCreateContext` + `ffxDispatch` de
   verdad con las cabeceras MIT de FidelityFX
   (`amd_mode/third_party/ffx_api/`) contra el DLL del pack.
4. **La ventana la crea el hilo que luego presenta.** El motor tarda ~3 s en
   levantarse, así que se levanta en un hilo aparte; pero si ese hilo crea la
   ventana y después muere, DXGI se bloquea dentro de `Present` esperando a un
   dueño que ya no bombea mensajes. De ahí `Prepare()` (ventana, hilo de
   frames) separado de `Start()` (lo lento, hilo de fondo).

## Cómo está cableado

- `amd_mode/native/dlssnr_engine.{h,cpp}`: hospeda el runtime. Carga dinámica
  (nada enlazado), `Prepare()` + `Start()`, `Dispatch()` por frame
  (upload BGRA → `ffxDispatch` → readback), `Resize()`, `Stop()`.
- `amd_mode/native/nr_host_full.cpp`: el worker. Levanta el motor **en un hilo
  de fondo** y sigue sirviendo passthrough mientras tanto — main da 5 s por
  frame antes de dar el worker por muerto, y levantar el motor cuesta más que
  eso repartido. Cuando está listo, el pase neuronal entra solo; la línea
  `[nr] … neural=N` cuenta los frames que pasaron por la red.
- Los formatos: el frame entra BGRA8, un pase de compute lo pasa a **fp16
  lineal** (lo que la red espera) y otro lo devuelve a sRGB en orden BGRA. La
  salida del dispatch es fp16 porque el runtime necesita *typed UAV store*
  para aplicar su residual, y B8G8R8A8 no lo garantiza.

## La quinta trampa: dónde coge el color, y los dos dispatch

El runtime toma su color de la **salida** del dispatch del upscaler, no de la
entrada. Medido: con la salida de FSR a 2560×1440 la red costaba 45–49 ms
pasara lo que pasara con el `renderSize`, y el slider de escala no hacía nada.

Pero hay una segunda frase suya que lo resuelve del todo: *"ignoring upscaler
dispatches without motion vectors … following the one with motion vectors"*.
Sigue el dispatch que lleva motion vectors e ignora los demás. De ahí el
diseño final, **dos dispatch por frame**:

| | resolución | motion vectors | quién lo procesa |
| --- | --- | --- | --- |
| **A** | trabajo → trabajo (1:1) | sí | la red neuronal |
| **B** | trabajo → salida | **no** | sólo FSR (el runtime lo ignora) |

Con eso: la red corre a la resolución de trabajo (lo que pide el slider) sobre
un frame **sin reescalar**, y el salto a la resolución del monitor lo hace FSR.
A escala 1.0 la resolución de trabajo *es* la de salida: B no existe y no se
reescala nada en todo el camino.

La versión intermedia —red a resolución de trabajo y un bilineal nuestro para
subir— es la que se veía mal: la red recibía un frame reducido, así que no
quedaba detalle fino que realzar, y encima el bilineal emborronaba el
resultado.

Medido sobre una foto nítida (1280×1704, detalle = energía de alta frecuencia):

| Escala | red corre a | detalle vs entrada | ms/frame |
| --- | --- | --- | --- |
| 1,00 | 1280×1704 | **1,10×** | 43,4 |
| 0,65 | 832×1104 | 0,90× | 30,1 |
| 0,50 | 640×848 | 0,99× | 25,5 |

## Medido aquí

En la app completa, monitor 2560×1440, escala 0,65 (red a 1664×936):

| Qué | Antes | Ahora |
| --- | --- | --- |
| FPS | 7,6 | **14,7** |
| `send` (Python → worker) | 14,2 ms | 2,3 ms (SHMI) |
| `recv` (worker → Python) | 92–131 ms | 41,6 ms (OUTS + los dos dispatch) |
| Red sola | 45–49 ms | 23 ms |
| Imagen | reescalada dos veces, sin detalle | sin reescalar para la red, upscale de FSR |

Y en el worker aislado: red 6 ms a 320×180, 36 ms el primer job (carga de
kernels); hooks del runtime ~450 ms; motor listo ~3,4 s (passthrough mientras
tanto). Referencia de campo: 15–16 ms/job a 720p en un juego real (log de RE
Requiem, 167 KB).

## Los canales de memoria compartida

El pipe costaba más que la red: 14,7 MB por frame en cada sentido a 1440p. El
worker AMD ahora acepta los dos canales que Python ya ofrecía y antes rechazaba
(`SACK`/`OAK2` con ok=0), con el mismo layout que el worker NVIDIA:

- **SHMI** (entrada): color + motion en una sección; el frame no viaja por el
  pipe. `send` 14,2 → 2,2 ms.
- **OUTS** (salida): los píxeles vuelven por otra sección con seqlock; `OUT1`
  lleva el centinela `0xFFFFFFFF`. El motor **escribe directamente** en la
  sección, sin copias intermedias.

`GRAY` sigue rechazado a propósito: es el canal de luminancia del modo DDA y
este worker no captura la pantalla por su cuenta.

## Por qué el pase "no se veía" (y por qué se veía mal)

Tres cosas distintas, encontradas midiendo, no mirando:

**1. Espacio de color.** El juego le da al runtime color **HDR lineal fp16**
(su log: `colour dxgi 10 … tonemap 1`); nosotros le dábamos sRGB de 8 bits
(`dxgi 28 … tonemap 0`). Con eso aplicaba una curva de tono que aplastaba las
luces (−48/255 en el extremo alto) mientras su aporte real era de 0,67/255.
Es decir: lo único visible era el daño. El pase pre/post ahora convierte
sRGB→lineal a la entrada y lineal→sRGB a la salida, y el runtime reporta
exactamente lo mismo que en el juego.

**2. La fuerza estaba al 3%.** `Scale` en el ini del runtime es la fuerza de
la red. El valor que traía la instalación era `0.03125`. Medido sobre una
imagen con detalle, aislando lo que aporta la red (quitando la curva de tono,
que sólo depende del valor del píxel):

| Scale | aporte de la red | veredicto |
| --- | --- | --- |
| 0.005 | 1,0/255 | invisible |
| 0.03 | 6,1/255 | **detalle real: piel, pelo, tejido** |
| 0.125 (tope del runtime) | 55/255 | halos, crujido, fringing |

**3. Los mandos del menú no llegaban.** El runtime no acepta parámetros por
el dispatch: los lee de su **propio ini**. Por eso en AMD los sliders no
hacían nada. `DlssNrEngine::SetEffect()` escribe ese ini antes de que el
runtime cargue y en cada cambio (RNSZ):

| Menú | ini del runtime |
| --- | --- |
| Intensity | `Scale` = intensity × 0.03 (`NS_AMD_NR_SCALE_MAX` mueve el tope) |
| Local tone | `LocalTone` |
| Local structure | `LocalStructure` |
| Skin structure | `SkinStructure` |
| Auto mask | `UseAutoMask` |

## Qué esperar del efecto

Lo que hace esta red es **iluminación y detalle de escena**: piel, pelo,
tejido, sombras de contacto. Sobre contenido fotográfico o de juego se nota;
sobre una UI plana no tiene casi nada que añadir — medido: 0,56/255 sobre una
captura de menú frente a 6,1/255 sobre una foto, con el mismo ajuste. No es un
fallo, es el dominio para el que se entrenó.

El salto de resolución lo hace FSR (los dos dispatch de arriba), no un
bilineal, así que bajar el slider cuesta detalle de forma suave en vez de
emborronar. A escala 1.0 no se reescala nada.

Y el ajuste que importa es `Scale` (el slider Intensity): 0,03 es donde el
detalle aparece sin artefactos; a 0,06 ya se nota forzado y a 0,125 (tope del
runtime) hay halos, piel naranja y fringing.

## Clics: no es un bug

Con el menú abierto la ventana se queda los clics a propósito (igual que
ReShade); el menú se abre solo al arrancar (`open_menu_on_start`). Cerrándolo
con Num2 el overlay vuelve a ser click-through y los clics caen en lo que haya
debajo — verificado con `WindowFromPoint` sobre el centro de la pantalla:
devuelve otra ventana, y el estilo del overlay lleva `TRANSPARENT|LAYERED|
NOACTIVATE`.

`docs/ab_baseline_amd_hip.json` es la baseline con red; `ab_baseline_amd.json`
sigue siendo la de passthrough. `tools/ab_compare.py --wait-neural 25` espera a
que el motor tome el relevo antes de medir (sin eso mides el passthrough).

## Tests

- `tests/test_amd_hip_engine.py` — la ruta neuronal: que el worker la tome,
  que la salida **cambie**, que siga siendo una imagen (tamaño, brillo, y R/B
  sin intercambiar), que el runtime loguee `engine init ok` y que la telemetría
  cuente frames neuronales. **Se salta solo** si no hay runtime instalado.
- `tests/test_amd_full_worker.py` — protocolo y entradas, fijado a
  `NS_AMD_NR=0` para que lea igual con motor o sin él.

## Lo que esto NO resuelve

- **Distribución.** El runtime es de un tercero y los pesos derivan del
  `nvngx_dlssnr.dll` de NVIDIA. Vale para uso propio; no puede ir en el ZIP de
  release. Por eso siguen vivos el pedido GPL
  (`docs/GPL_SOURCE_REQUEST.md`) y el plan B ONNX/DirectML.
- **El autor.** El backend HIP es de **Danielblnc**, no de wilsjo2, que lo
  redistribuye. El pedido de fuentes debería incluirlo.
- **Frame Generation en AMD**: sigue diferido (necesita Streamline).
