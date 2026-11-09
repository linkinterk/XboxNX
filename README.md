# XboxNX

Port del emulador de Xbox original **hakuX** a Nintendo Switch, nativo (homebrew
.nro, sin Android). Se hace por etapas.

| Etapa | Qué hace | Estado |
|---|---|---|
| 1. Cargador | Biblioteca con la imagen de cada juego, lee la ISO, analiza `default.xbe` y prueba el hardware | Lista |
| 2a. Memoria | 64 MB del Xbox, secciones del XBE en sus direcciones (verificadas con SHA-1), llamadas al kernel conectadas | **Esta versión** |
| 2b. CPU | Traductor x86→ARM64 (FEX) sobre el JIT de la Switch | Pendiente |
| 2c. Arranque | Ejecutar las primeras instrucciones hasta la primera llamada al kernel | Pendiente |
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

En el detalle de un juego, **Y** lo carga en la memoria del Xbox (etapa 2a).

Controles: **A** o tocar la tarjeta: abrir · **L/R** cambiar pestaña · **X**
pruebas de hardware · **Y** buscar juegos otra vez · **B** volver · **+** salir.

En la pantalla de pruebas (X), pulsa **A** para la prueba de excepciones y
manda una foto del resultado: decide cómo se hace la etapa 2.

## Vista previa en la PC

`tools/preview.c` dibuja todas las pantallas a PNG con juegos de ejemplo
inventados (ver el comentario del archivo).

## Compilar

Lo compila GitHub Actions en cada push (`.github/workflows/build.yml`). A mano,
con devkitPro instalado: `make`.

## Licencia

GPL-3.0-or-later (ver `COPYING`). Basado en
[hakuX](https://github.com/rfandango/hakuX) de rfandango (GPL-3.0-or-later), que
a su vez toma partes de xemu. Texto con stb_truetype (dominio público / MIT).
El ícono es original. No incluye software de Microsoft: necesitas tus
propias imágenes de disco.
