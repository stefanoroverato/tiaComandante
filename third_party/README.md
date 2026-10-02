# Librerie di terze parti

Ogni libreria è un **git submodule** bloccato su un commit preciso: CMake compila direttamente i sorgenti del submodule.

| Cartella | Libreria | Licenza | Versione (tag) | Commit | URL attuale |
|---|---|---|---|---|---|
| `cjson/` | cJSON | MIT | v1.7.19 | `c859b25da02955fef659d658b8f324b5cde87be3` | upstream `https://github.com/DaveGamble/cJSON.git` |
| `mxml/` | Mini-XML | Apache-2.0 | v4.0.6 | `874e249a0d3b506e883210b7ceece5316a8489d4` | upstream `https://github.com/michaelrsweet/mxml.git` |

Dai sorgenti si usano solo i file seguenti:
- cJSON: `cJSON.c`, `cJSON.h`;
- Mini-XML: `mxml-*.c`, `mxml-private.h`, `mxml.h`, `vcnet/config.h`.

Mini-XML è compilato come libreria statica senza `MXML1_EXPORTS`. Il suo stato globale quindi non è thread-safe, e tiaComandante lo usa solo dal thread worker.

## Passaggio ai fork interni

Quando i fork esistono, si cambia solo l'URL; il commit fissato resta lo stesso:

```bash
git submodule set-url third_party/cjson <URL-fork-cJSON>
git submodule set-url third_party/mxml  <URL-fork-mxml>
git submodule sync
git submodule update --init
git commit -am "third_party: use internal forks"
```

Le modifiche locali alle librerie si fanno nei fork e si annotano qui sotto.

## Modifiche locali

Nessuna.
