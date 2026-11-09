# XboxNX

Port del emulador de Xbox original **hakuX** a Nintendo Switch, nativo (homebrew
.nro, sin Android). Se hace por etapas.

| Etapa | Qué hace | Estado |
|---|---|---|
| 1. Cargador | Lee la ISO de la SD, abre el disco de Xbox, analiza `default.xbe` y prueba el hardware | **Esta versión** |
| 2. CPU y memoria | Traductor x86→ARM64 (FEX) sobre el JIT de la Switch, memoria del Xbox | Pendiente |
| 3. Kernel, audio, controles | Kernel de Xbox de hakuX, audio MCPX, controles | Pendiente |
| 4. Gráficos | Renderizador NV2A de Vulkan a deko3d | Pendiente |
| 5. Optimizar | Velocidad en el CPU de la Switch | Pendiente |

## Cómo usar la etapa 1

1. Descarga `XboxNX.nro` de la pestaña **Actions** (el último build verde,
   sección *Artifacts*) o de **Releases**.
2. Cópialo a `sd:/switch/XboxNX.nro`.
3. Copia tu ISO de Xbox (de un disco tuyo, `.iso` o `.xiso`) a `sd:/xbox/`.
   Si pesa más de 4 GiB la SD tiene que estar en **exFAT**.
4. Abre el Homebrew Menu **manteniendo R sobre un juego** (modo aplicación,
   más RAM) y entra a XboxNX.

Controles: **A** abrir juego · **L/R** cambiar página · **X** pruebas de
hardware · **B** volver · **+** salir.

En la pantalla de pruebas (X), pulsa **A** para la prueba de excepciones y
manda una foto del resultado: decide cómo se hace la etapa 2.

## Compilar

Lo compila GitHub Actions en cada push (`.github/workflows/build.yml`). A mano,
con devkitPro instalado: `make`.

## Licencia

GPL-3.0-or-later (ver `COPYING`). Basado en
[hakuX](https://github.com/rfandango/hakuX) de rfandango (GPL-3.0-or-later), que
a su vez toma partes de xemu. No incluye software de Microsoft: necesitas tus
propias imágenes de disco.
