# Port de FFmpeg

libavutil / libavcodec / libavformat / libswresample / libswscale compiladas
contra la libc de SavanXP, con dos consumidores que corren adentro del SO.

Estado: **reproduce video**. `build.ps1 ffmpeg-smoke` corre tres cosas:

| binario | que hace |
| --- | --- |
| `wavinfo` | abre un WAV, informa lo que hay adentro y lo decodifica entero |
| `player --selftest` | decodifica MJPEG y convierte YUV->RGB con swscale, sin pantalla |
| `player --hold` | lo mismo pero presentando por `/dev/gpu0`, y deja el ultimo cuadro fijo para que el harness saque una captura |

El set de codecs es a proposito acotado: WAV/PCM s16le y MJPEG. MJPEG y no
H.264 porque este build va sin asm y sin hilos, y porque un stream MJPEG crudo
son JPEGs concatenados -- se genera sin muxear nada.

## Requisitos

FFmpeg se construye con GNU make, que el pipeline de SavanXP no usa para nada
mas. Asi que el port se construye aparte, en un entorno POSIX que tenga:

- **GNU make**
- **clang** y **ld.lld** (cualquier version reciente sirve)
- shell POSIX, `curl`, `tar`

En Linux o macOS eso ya esta. En Windows hace falta un entorno que lo provea:
WSL y MSYS2 sirven los dos. Los scripts derivan la raiz del repo de su propia
ubicacion, asi que no hay rutas que ajustar.

Que se use OTRO clang que el de `toolchain/` no parte el build en dos: se
compila con los mismos flags de `tools/UserAppCommon.ps1` y para el mismo
`-target x86_64-unknown-none-elf`, asi que lo que sale son objetos ELF x86-64
freestanding, iguales a los que produce el clang horneado. La division es:

- el entorno POSIX produce `build/external/wavinfo.elf` (los `.a` de FFmpeg
  quedan en su directorio de trabajo, no cruzan);
- `build.ps1 ffmpeg-smoke` lo instala en la imagen y lo corre.

El fuente de FFmpeg no entra al repo: se baja y se construye en el directorio de
trabajo (`$HOME/savanxp-ffmpeg` por default, se cambia con `WORK=`). Si ese
directorio cae sobre un filesystem montado desde otro sistema -- por ejemplo
`/mnt/...` en WSL -- el build tarda un orden de magnitud mas, porque son decenas
de miles de operaciones de archivo. Conviene dejarlo en el filesystem nativo.

## Uso

```
bash sdk/ffmpeg/all.sh          # en el entorno POSIX
python sdk/ffmpeg/make-tone.py  # genera build/media/tono.wav
python sdk/ffmpeg/make-clip.py  # genera build/media/clip.mjpeg
build.ps1 ffmpeg-smoke          # instala y corre adentro del SO
```

La captura del player queda en `build/shots/player/`.

`all.sh` encadena los pasos, que tambien se pueden correr sueltos:

| script | que hace |
| --- | --- |
| `fetch.sh` | baja y desempaqueta FFmpeg (version fijada adentro) |
| `runtime.sh` | compila el runtime de SavanXP como `libsavanxp.a` |
| `configure.sh` | corre el configure de FFmpeg apuntando al target |
| `build.sh` | `make -k` y resume que fallo |
| `link-demo.sh` | compila y linkea `wavinfo.c` y `player.c` contra las libav* |

El material de prueba lo generan `make-tone.py` (un WAV de 440 Hz) y
`make-clip.py` (24 cuadros MJPEG). El clip esta dibujado para que un error se
VEA: una barra que avanza un paso por cuadro, franjas de color puro -- si los
planos U/V se cruzan cambian de color -- y el numero de cuadro.

## Las tres cosas que no son obvias

Las tres comparten la misma forma: no fallan ruidosamente, producen una
configuracion equivocada.

1. **`runtime.sh` existe porque el configure de FFmpeg LINKEA.** Sin una libc
   contra la que linkear, sus checks no dan error: concluyen que la libc no
   tiene nada y siguen.

2. **`-no-pie` es obligatorio.** El driver de clang le pasa `-pie` al linker por
   default, y con `-fno-pic` lld rechaza cualquier programa por las
   reubicaciones absolutas. El sintoma no es un error de link sino un configure
   que decide que no existe `trunc`.

3. **`-static` es obligatorio.** Sin el, el driver marca el ELF como dinamico y
   agrega `.interp`, que empuja al segundo `PT_LOAD` a una direccion sin alinear
   a pagina.

## Como se verifica lo visual

"Se ve bien" no es algo que un harness pueda asertar, asi que esta partido en
dos. El `--selftest` cubre lo comprobable sin ojos: que todos los cuadros
decodifiquen, que mantengan el tamano declarado y que la conversion produzca
pixeles que no sean todos iguales. El `--hold` cubre lo otro -- que los pixeles
LLEGUEN a la pantalla --: deja el ultimo cuadro fijo, avisa por serial, y ahi el
harness saca una captura por QMP y comprueba que no sea de un solo color.

El player presenta por `/dev/gpu0` (`gpu_open`/`gpu_acquire`/`gpu_present`) y no
por `gfx_open`: ese es el camino de un cliente del WM, que mapea un fd heredado
de windowd. Un proceso lanzado por init -- que es como corre en el harness -- no
lo tiene. Es el mismo camino que usa `gputest`.

## Que falta para subir de codecs

El set se agranda en `configure.sh` (`--enable-decoder=`, `--enable-demuxer=`).
Lo que probablemente aparezca al hacerlo:

- Mas superficie de libc. Lo que ya se sabe que falta: `fscanf`, `mkstemp`,
  `realpath`.
- Rendimiento: el build va sin asm (`--disable-asm`) y sin hilos
  (`--disable-pthreads`), porque el kernel no tiene primitiva de hilos y el
  toolchain no trae nasm.
- Tamano: `wavinfo` pesa ~700 KB con un solo codec PCM. La imagen SxFS son 64
  MiB y 255 inodos, asi que un set grande hay que medirlo.
