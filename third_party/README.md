# Librerie di terze parti

Ogni libreria è un **git submodule** che punta a un fork interno ed è bloccato su un commit preciso: CMake compila direttamente i sorgenti del submodule.

| Cartella | Libreria | Licenza | Versione | Commit | Fork | Upstream |
|---|---|---|---|---|---|---|
| `cjson/` | cJSON | MIT | v1.7.19 | `c859b25da02955fef659d658b8f324b5cde87be3` | `https://github.com/stefanoroverato/cJSON.git` | `https://github.com/DaveGamble/cJSON.git` |
| `mxml/` | Mini-XML | Apache-2.0 | v4.0.6 | `874e249a0d3b506e883210b7ceece5316a8489d4` | `https://github.com/stefanoroverato/mxml.git` | `https://github.com/michaelrsweet/mxml.git` |

Dai sorgenti si usano solo i file seguenti:
- cJSON: `cJSON.c`, `cJSON.h`;
- Mini-XML: `mxml-*.c`, `mxml-private.h`, `mxml.h`, `vcnet/config.h`.

Mini-XML è compilato come libreria statica senza `MXML1_EXPORTS`. Il suo stato globale quindi non è thread-safe, e tiaComandante lo usa solo dal thread worker.

## Uso

```bash
git clone --recurse-submodules https://github.com/stefanoroverato/tiaComandante.git
git submodule update --init          # dopo un pull, o in un clone fatto senza --recurse-submodules
```

## Aggiornare una libreria a una nuova versione upstream

```bash
cd third_party/cjson
git remote add upstream https://github.com/DaveGamble/cJSON.git   # solo la prima volta
git fetch upstream --tags
git checkout <tag-o-commit>
git push origin HEAD:master        # porta il commit nel fork, se non c'è già
cd ../..
git add third_party/cjson
git commit -m "third_party: cJSON <versione>"
```

Prima del commit nel progetto principale, ricompila ed esegui i test. Aggiorna anche la tabella qui sopra.

## Modifiche locali

Nessuna. Le modifiche si fanno nei fork (commit e push nel submodule, poi `git add` del submodule nel progetto principale) e si annotano qui.
