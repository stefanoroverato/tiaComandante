# Librerie di terze parti

Ogni libreria è un **git submodule** che punta a un fork interno ed è bloccato su un commit preciso: CMake compila direttamente i sorgenti del submodule.

| Cartella | Libreria | Licenza | Versione | Commit | Fork | Upstream |
|---|---|---|---|---|---|---|
| `cjson/` | cJSON | MIT | v1.7.19 | `c859b25da02955fef659d658b8f324b5cde87be3` | `https://github.com/stefanoroverato/cJSON.git` | `https://github.com/DaveGamble/cJSON.git` |
| `mxml/` | Mini-XML | Apache-2.0 | v4.0.6 | `874e249a0d3b506e883210b7ceece5316a8489d4` | `https://github.com/stefanoroverato/mxml.git` | `https://github.com/michaelrsweet/mxml.git` |
| `s7commplus/` | S7CommPlusDriver (+ ZLIB.NET, OpenSSL 3 DLL) | LGPL-3.0-or-later (ZLIB.NET: BSD, OpenSSL: Apache-2.0) | master del 2026-03-24 + modifiche locali | `43334e1ab746907eb45d0e74a7a9f8872a9961e3` (sopra `dbd61e4`) | `https://github.com/stefanoroverato/S7CommPlusDriver.git` | `https://github.com/thomas-v2/S7CommPlusDriver.git` |

Testi delle licenze e avvisi da riportare con i binari: [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

Dai sorgenti si usano solo i file seguenti:
- cJSON: `cJSON.c`, `cJSON.h`;
- Mini-XML: `mxml-*.c`, `mxml-private.h`, `mxml.h`, `vcnet/config.h`.

Mini-XML è compilato come libreria statica senza `MXML1_EXPORTS`. Il suo stato globale quindi non è thread-safe, e tiaComandante lo usa solo dal thread worker.

S7CommPlusDriver (dati live) **non** è compilato dentro tiaComandante: CMake compila `src/Zlib.net/*.cs` e `src/S7CommPlusDriver/*.cs` in due assembly separati (`zlib.net.dll`, `S7CommPlusDriver.dll`, C# 7.3 con il `csc.exe` Roslyn di Visual Studio / Build Tools) e li copia accanto al server insieme alle DLL di OpenSSL 3 (`src/S7CommPlusDriver/OpenSSL-dll-x64`, versione 3.0.8, oppure la cartella indicata in `TC_OPENSSL_DIR`) e ai testi delle licenze. Il server li carica a runtime nel bridge .NET e usa solo l'interfaccia pubblica del driver. Con `-DTC_LIVE_DATA=OFF` il server si compila senza dati live. Le DLL di OpenSSL nel repository del driver non sono firmate e la 3.0.8 è vecchia: per una distribuzione conviene puntare `TC_OPENSSL_DIR` a una build ufficiale aggiornata di OpenSSL 3.

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

Le modifiche si fanno nei fork (commit e push nel submodule, poi `git add` del submodule nel progetto principale) e si annotano qui.

cJSON e Mini-XML: nessuna.

S7CommPlusDriver (fork `stefanoroverato/S7CommPlusDriver`, sopra `dbd61e4`):
- **Log delle chiavi TLS solo su richiesta** (`Net/S7Client.cs`): l'originale scrive a ogni connessione i segreti della sessione TLS in `key_<data>.log` nella cartella corrente (per Wireshark). Ora il file si scrive solo se `S7Client.KeyLogDirectory` è impostato; tiaComandante non lo imposta.
- **Identità del PLC** (`S7CommPlusConnection.cs`): proprietà `SessionVersionPAOMString` con la stringa di identificazione ricevuta all'apertura della sessione (codice d'ordine e firmware, es. `1;6ES7 511-1AK02-0AB0 ;V2.9`).
- **Livello di protezione** (`Legitimation/Legitimation.cs`): metodo pubblico `GetEffectiveProtectionLevel(out UInt32)`, usato anche dalla legittimazione al posto del codice duplicato.
- **Timeout di connessione** (`Net/MsgSocket.cs`, `Net/S7Client.cs`, `S7CommPlusConnection.cs`): la connessione TCP usa il timeout passato a `Connect` (prima un host irraggiungibile bloccava per circa 21 s).

Sono modifiche piccole e additive, candidate a una pull request verso `thomas-v2/S7CommPlusDriver`.
