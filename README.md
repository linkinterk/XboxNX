# XboxNX

Port del emulador de Xbox original **hakuX** a Nintendo Switch, nativo (homebrew
.nro, sin Android). Se hace por etapas.

| Etapa | Qué hace | Estado |
|---|---|---|
| 1. Cargador | Biblioteca con la imagen de cada juego, lee la ISO, analiza `default.xbe` y prueba el hardware | Lista |
| 2a. Memoria | 64 MB del Xbox, secciones del XBE en sus direcciones (verificadas con SHA-1), llamadas al kernel conectadas | Lista |
| 2b. CPU (intérprete) | Intérprete x86 propio con x87, MMX y SSE del Pentium III, probado contra Unicorn | Lista |
| 2c. Arranque | Hilo de arranque como el kernel del Xbox; registro de cada llamada al kernel | Lista |
| 2d. CPU (FEX) | Traductor x86→ARM64 sobre el JIT de la Switch, para la velocidad | Pendiente |
| 3a. Kernel: memoria y objetos | Memoria contigua y virtual, pool, pilas, handles, eventos, semáforos, mutex (44 funciones reales) | Lista |
| 3b. Kernel: hilos y tiempo | Varios hilos con prioridades y turnos, esperas, secciones críticas, temporizadores, DPC, tiempo virtual (97 funciones reales) | Lista |
| 3c. Kernel: archivos | D: desde la ISO, disco duro del Xbox en `sd:/switch/XboxNX/hdd`, carpetas, enlaces, dispositivos (130 funciones reales) | Lista |
| 3d. Kernel: sistema | Rtl (cadenas, memoria, tiempo), configuración de la consola (EEPROM), cerrojos, secciones del XBE, HAL, AV, criptografía Xc (199 funciones reales) | Lista |
| 3e. Hardware simulado | Registros del GPU NV2A (temporizador, vblank, interrupción, cola de comandos sin dibujar), USB, audio y red como registros, puertos de E/S, interrupciones y su hilo | Lista |
| 3f. Audio | El APU de hakuX/xemu: procesador de voces y DSP56300 (GP y EP) a tiempo virtual, AC97 (el sonido aún no sale por la Switch) | Lista |
| 4a. Pantalla | La imagen que el Xbox muestra (botón X mientras corre), cuadros mostrados, borrado de superficies (CLEAR_SURFACE) por software y lo que cada cuadro pide dibujar | Lista |
| 4b. Dibujo por software | Triángulos con el vertex shader del juego (nv2a_vsh_cpu), profundidad, texturas (swizzle, lineales, DXT, YUV), register combiners y mezcla, en la CPU de la Switch | Lista |
| 3g. Controles | El USB del Xbox (OHCI con hub y control XID, de hakuX) a tiempo virtual; en la pantalla del Xbox los botones de la Switch son los del control (L3 + R3 vuelve al registro) | Lista |
| 5a. Optimización 1 | Intérprete ~40% más rápido (página de código guardada), dibujo por software en 3 núcleos, menos redibujos de la pantalla, reloj virtual ajustable (Y) y la tabla de etapas en la app (botón − en la biblioteca) | Lista |
| 5b. Optimización 2 | Flags perezosos (se calculan solo cuando una instrucción los lee), probados con 40.000 pares contra Unicorn: el intérprete pasó de 47 a ~80 millones de instrucciones por segundo en la PC | Lista |
| 5c. Optimización 3 | El chip de audio en su propio hilo (núcleo 1), en paralelo al intérprete como el DSP real; la medida del tiempo real en el registro | Lista |
| 2d.1. Bloques decodificados | Cada trozo de código x86 se decodifica una vez en un bloque de instrucciones ya entendidas y se ejecuta sin volver a leer los bytes (las que no tienen función propia las hace el intérprete); escribir en una página de código la vuelve a decodificar. ~1,8x en la PC. Además, los NaN de x87 y SSE como en x86 también en ARM64 (probado con qemu). v0.17.1: las MMX del video Bink con función propia (2,4x en ese código) y las x87/SSE/mul/div/rep ya no cortan el bloque. v0.17.2: arreglado el int3 en 0x80010F14 (un hilo que esperaba durante un DPC guardaba los registros del DPC). v0.17.3: las interrupciones se atienden aunque el hilo esté a DISPATCH_LEVEL (D3D esperaba al GPU así y el juego se quedaba en el logo). v0.17.5: tras un ISR se atiende la interrupción pendiente de más IRQL (el USB, cada 1 ms, dejaba sin turno al ISR del GPU y su trampa nunca se liberaba); Halo 2 ya pasa del logo al video | Lista |
| 2d.3. JIT ampliado | v0.18.32: los bloques traducidos se encadenan: al terminar, uno busca el siguiente en una tabla y salta a su código sin volver a run_blocks (cada entrada comprueba presupuesto, interrupción, página y generación del código); las Insn van copiadas junto al código. ~2x en los bucles de prueba (entero 30 -> 61, MMX 91 -> 163, x87+SSE 54 -> 80 MIPS con qemu). FPS reales y velocidad del juego en pantalla. v0.18.31: alu/test/inc/dec de 32 bits con memoria, call/jmp/push indirectos (FF), jcc/setcc/cmov con flags de antes del bloque y una línea "Tiempo CPU" en la pantalla del Xbox que mide dónde se va el tiempo (código traducido, bloques sin traducir, intérprete, buscar bloque, llamadas a C). v0.18.30: x87 (cargar, guardar, aritmética, fxch, fchs, fabs, fild, fist/fistp con el redondeo del cw, fldcw/fnstcw, comparaciones y fnstsw) y SSE (movups/movaps/movss, add/sub/mul/div/min/max en ps y ss, and/andn/or/xor, unpck, shufps, comiss, cvttss2si, prefetch) en código nativo con NEON; inc/dec, shl/shr/sar con flags perezosos, jmp/call/ret y el jcc tras inc/dec en código nativo. Lo raro (un NaN, fuera del rango de float con 24 bits, otro modo de redondeo) lo hace la función de siempre. Bucle x87+SSE de prueba ~19x, bucle entero ~2x | **Esta versión** |
| 2d.2. JIT a ARM64 | Cada bloque que se repite se traduce a una función ARM64 (en la Switch con el JIT de libnx): mov, add/sub/cmp/and/or/xor, test, lea, push, pop y cargas/guardados de 32 bits con la tabla de páginas en línea van en código nativo; las demás llaman a su función. Probado con qemu contra el intérprete y Unicorn. ~1,8x en el bucle de prueba. v0.18.24: las escrituras a la SD (Halo 2 copia el mapa a su caché al cargar, ~10 ms cada una) se hacen en un hilo aparte: el juego ya no las espera. v0.18.23: tiempo real por función del kernel. v0.18.22 (2d.3): jcc tras cmp/sub/test/and/or/xor con los flags de ARM64, movzx/movsx y mov de 8 y 16 bits en código nativo. v0.18.21 (2d.3): MMX en el JIT con NEON (sumas, restas con saturación, multiplicaciones, comparaciones, lógicas, unpack/pack, desplazamientos, movd/movq): ~4.7x en el bucle MMX de prueba; el perfil ya no cuenta como lentas las que solo tienen camino de respaldo. v0.18.20: tabla de bloques 8 veces más grande y búfer del JIT de 128 MB (Halo 2 los vaciaba y retraducía todo), y un perfil del CPU en la pantalla del Xbox con las instrucciones que se quedan fuera del JIT. v0.18.19: más rápido: salta cuadros (dibuja 1 de cada 3 por defecto; ◀ ▶ en el registro lo cambia), los triángulos de un dibujo se pintan juntos en los 3 núcleos, los vértices se transforman en paralelo y el dibujo se compila con -O3. v0.18.18: los triángulos que cruzan la cámara se recortan contra el plano cercano (antes salían como franjas estiradas en los niveles); R3 apaga/prende el JIT y L3 los bloques, para probar si un error es del JIT. v0.18.17: mapas cúbicos y texturas con paleta (P8); − también captura el cuadro siguiente entero (volcado/cuadro.bin y cuadro_ram.bin) para repetirlo en la PC con tools/replay.c. v0.18.16: si PCRTC apunta a una imagen negra se muestra el último cuadro dibujado (el menú de Halo 2 ya se ve). v0.18.15: las texturas swizzle de cada etapa tienen su propia tabla (antes una textura de otro tamaño liberaba la de otra etapa y se leían direcciones basura: cierre al dibujar el menú de Halo 2); si XboxNX falla, se detiene el juego con el informe en pantalla y en sd:/switch/XboxNX/cierre.txt en vez de cerrarse. v0.18.13: el tamaño de un archivo abierto se lee del archivo mismo (en la Switch stat() de un archivo abierto para escribir falla y daba 0: Halo 2 no podía leer su caché y quedaba en negro tras la intro). v0.18.1: la trampa del GPU se atiende en cuanto ocurre (antes esperaba al final del turno, ~16 ms por trampa). v0.18.2: los bucles que solo esperan (leen y comparan con los mismos registros) saltan el tiempo al siguiente evento en vez de dar millones de vueltas. v0.18.7: escribir datos en una página con código ya no obliga a traducirlo otra vez (mapa de bytes de código por página), tabla de bloques 4x más grande y JIT a la cuarta vez | Hecha |
| 3h. Sonido | El audio del Xbox por la Switch | Pendiente |
| 4c. Gráficos | Lo mismo en el GPU de la Switch (OpenGL 3.3 con mesa/nouveau): un shader que hace los combiners, superficies como texturas y copia a la RAM al mostrar. L3 cambia entre GPU y CPU | En curso |
| 5. Optimizar | Velocidad en el CPU de la Switch | Pendiente |

## Cómo usar la etapa 1

1. Descarga `XboxNX.nro` de la pestaña **Actions** (el último build verde,
   sección *Artifacts*) o de **Releases**.
2. Cópialo a `sd:/switch/XboxNX.nro`.
3. Copia tu ISO de Xbox (de un disco tuyo, `.iso` o `.xiso`) a `sd:/xbox/`.
   Si pesa más de 4 GiB la SD tiene que estar en **exFAT**.
4. Abre el Homebrew Menu **manteniendo R sobre un juego** (modo aplicación,
   más RAM) y entra a XboxNX.

En el detalle de un juego, **Y** lo carga en la memoria del Xbox (etapa 2a); ahí, **A**
ejecuta su código en el intérprete y muestra cada llamada al kernel (etapa 2c).

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
