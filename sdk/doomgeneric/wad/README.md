# IWADs

Ningún WAD se versiona en el repo: `.gitignore` excluye `wad/*.wad`. Este
directorio es solo la ubicación donde el build espera encontrarlo.

El IWAD por defecto es Freedoom, un IWAD libre bajo BSD-3-Clause sin relación
con id Software, que permite jugar sin contenido propietario ni shareware.
Descargalo de https://freedoom.github.io/download.html y dejalo acá:

```text
sdk/doomgeneric/wad/freedoom1.wad
```

Para usar otro WAD sin copiarlo al repo:

```powershell
.\sdk\doomgeneric\build.ps1 -WadPath C:\ruta\a\doom1.wad
```

El script instala el IWAD elegido en `/disk/games/doom` dentro de la imagen, así
que la imagen construida sí redistribuye el WAD. Su procedencia está registrada
en `docs/THIRD_PARTY_PROVENANCE.md`. Si el archivo falta, el build sigue sin
instalarlo.
