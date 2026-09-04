# Port de FFmpeg

libavutil / libavcodec / libavformat / libswresample compiladas contra la libc
de SavanXP, y un consumidor (`wavinfo`) que corre adentro del SO.

Estado: **decodifica**. `build.ps1 ffmpeg-smoke` abre un WAV desde `/disk`,
identifica el formato, elige el decoder, decodifica todos los frames y reporta
la duracion. El set de codecs habilitado es a proposito minimo (WAV + PCM
s16le): lo que se estaba probando es que la cadena entera existe, no cuantos
formatos entran.

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
build.ps1 ffmpeg-smoke          # instala y corre adentro del SO
```

`all.sh` encadena los pasos, que tambien se pueden correr sueltos:

| script | que hace |
| --- | --- |
| `fetch.sh` | baja y desempaqueta FFmpeg (version fijada adentro) |
| `runtime.sh` | compila el runtime de SavanXP como `libsavanxp.a` |
| `configure.sh` | corre el configure de FFmpeg apuntando al target |
| `build.sh` | `make -k` y resume que fallo |
| `link-demo.sh` | compila y linkea `wavinfo.c` contra las libav* |

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

## Que falta para subir de codecs

El set se agranda en `configure.sh` (`--enable-decoder=`, `--enable-demuxer=`).
Lo que probablemente aparezca al hacerlo:

- Mas superficie de libc. Lo que ya se sabe que falta: `fscanf`, `mkstemp`,
  `realpath`.
- Rendimiento: el build va sin asm (`--disable-asm`) y sin hilos
  (`--disable-pthreads`), porque el kernel no tiene primitiva de hilos y el
  toolchain no trae nasm.
- Tamano: `wavinfo` pesa ~700 KB con un solo codec PCM. La imagen SVFS2 son 64
  MiB y 255 inodos, asi que un set grande hay que medirlo.
