# tiaComandante

Server MCP (Model Context Protocol) per **Siemens TIA Portal V21**, scritto in **C** e basato sulle API Openness V21.
Espone gli stessi tool di TiaCommander (un tool per area, con parametro `action`), così prompt e workflow già in uso restano validi.

- Trasporto: JSON-RPC 2.0 su stdio (versioni MCP 2024-11-05 … 2025-11-25)
- Piattaforma: Windows x64, TIA Portal V21 con Openness, .NET Framework 4.8
- Dipendenze di terze parti: cJSON (MIT) e Mini-XML 4 (Apache-2.0), come git submodule in `third_party/` (vedi `third_party/README.md`)

## Architettura

```
client MCP ──stdio──► tiacomandante.exe ──► tiacomandante-core.dll (C)
                                               │  CLR v4 ospitato (mscoree)
                                               ▼
                                     TiaComandante.Bridge.dll (C# 5, reflection generica)
                                               │  AssemblyResolve → PublicAPI\V21\net48
                                               ▼
                                     Siemens.Engineering.* ──► TIA Portal V21
```

- **`tiacomandante.exe`** è un launcher minimo e stabile, con build deterministica. L'AllowList di Openness identifica l'applicazione con l'hash SHA-256 dell'exe: tenendo l'exe invariato, l'accesso si approva una sola volta. Tutta la logica sta in `tiacomandante-core.dll`.
- **Il bridge .NET** non contiene logica di dominio e non referenzia gli assembly Siemens. Offre al C solo: handle sugli oggetti, get/set/call con conversione dei tipi guidata dalla firma, `GetService<T>`, enumerazioni, eventi e delegate richiamati in C, eccezioni complete. Lo compila CMake con il `csc.exe` incluso in .NET Framework.
- **Il thread worker** (STA) è l'unico a usare Openness: le `tools/call` vengono eseguite in serie, mentre `ping` e `tools/list` rispondono subito.
- **Le modifiche** a DB, UDT, interfacce, reti e watch table seguono il ciclo export SimaticML → modifica (Mini-XML) → reimport.

## Build

Requisiti: CMake ≥ 3.20, Visual Studio 2022 o Build Tools (MSVC x64), .NET Framework 4.8, git.

```powershell
git clone --recurse-submodules <url-repository> tiaComandante   # oppure, in un clone esistente:
git submodule update --init
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release        # test di protocollo MCP (non serve TIA)
```

In `build\Release\` vengono prodotti `tiacomandante.exe`, `tiacomandante-core.dll` e `TiaComandante.Bridge.dll`, che vanno copiati insieme nella stessa cartella. La CRT è statica, quindi non serve il redistributable VC++.

## Installazione

1. Installa TIA Portal V21 con l'opzione **Openness** e aggiungi il tuo utente Windows al gruppo locale **"Siemens TIA Openness"** (poi esegui logout e login).
2. Copia i tre file in una cartella stabile, ad esempio `C:\TiaComandante\`.
3. Al primo collegamento TIA Portal chiede di consentire l'accesso Openness a `tiacomandante.exe`: conferma. L'approvazione vale finché l'exe e il suo percorso non cambiano. Aggiornare `tiacomandante-core.dll` non richiede una nuova approvazione.
4. Registra il server nel client MCP (vedi `docs/configs/`):

   ```powershell
   claude mcp add tiacomandante -- C:\TiaComandante\tiacomandante.exe
   ```

## Uso

Workflow tipico:

1. `session action=get_state`
2. `session action=connect` (aggancia il TIA Portal V21 in esecuzione) oppure `open`/`create`
3. `session action=list_devices`
4. gli altri tool, con il `deviceName` ricavato al passo 3
5. `session action=save`

| Tool | Contenuto |
|---|---|
| `get_info` | versione, rilevamento di TIA/Openness, stato della connessione |
| `session` | stato, connect/launch/open/create, save/save_as/archive/close, elenco device, cartelle predefinite |
| `admin` | archivio degli export (exportId, 24 h), statistiche, errori recenti, informazioni di sistema, open_file |
| `blocks_read` | elenco, dettagli, interfacce, reti (SCL come testo, LAD/FBD come elementi), export XML/sorgente, compilazione |
| `blocks_write` | creazione FB/FC/OB (SCL da codice, LAD/FBD con reti vuote), import XML, copy/move/rename/delete, editor di interfaccia con blast radius, multi-istanze, reti |
| `db` | DB globali e di istanza: struttura, scheda membro, membri, start value (anche di elementi di array e di DB di istanza) |
| `udt` | tipi di dati PLC: struttura, membri, import/export, spostamento |
| `tag` | tabelle e tag: commenti, flag di accesso, move/rename, ricerca, CSV/XML, occupazione indirizzi e primo indirizzo libero |
| `folders` | albero delle cartelle, ricerca glob, creazione, eliminazione (con conferma) e rinomina |
| `xref` | riferimenti, chiamanti (di blocchi e di tag), albero e percorsi di chiamata, blocchi inutilizzati, DB di istanza orfani |
| `watch` | watch e force table: voci, CSV/XML, rename/move |
| `diagnostics` | IP, configurazione della connessione, online/offline, scansione di rete, confronto online/offline |
| `download_upload` | download con pre-check di compilazione e conferma esplicita, upload della stazione |

Sicurezza:
- Le azioni che modificano il progetto **non si collegano mai in automatico** a un TIA in esecuzione: il target va scelto in modo esplicito con `session connect/open/create`.
- Le operazioni distruttive richiedono una frase di conferma: `download_to_device` vuole `confirm='I understand this will modify the PLC'`.
- **Modalità sola lettura:** con `--read-only`, con `TIACMD_READONLY=1` o con `"readOnly": true` nel file `config.json`, ogni azione di scrittura viene rifiutata.

File:
- Configurazione: `%APPDATA%\tiaComandante\config.json`
- Log ed export: `%LOCALAPPDATA%\tiaComandante\`

## Riga di comando

```text
tiacomandante                         server MCP su stdio
tiacomandante --call TOOL '{json}'    esegue una singola chiamata e stampa il risultato
tiacomandante --selftest              verifica CLR, bridge e Openness con un TIA Portal V21 aperto
tiacomandante --members TIPO          elenca i membri di un tipo Openness (aiuto allo sviluppo)
tiacomandante --read-only | --log-level debug|info|warn|error
```

## Sviluppo e test

- `tests/test_mcp.c` (CTest) verifica handshake, schemi, errori JSON-RPC e chiusura pulita.
- `tests/dev/mcpdrive.py` è un driver opzionale (richiede Python) che esegue sequenze di chiamate in un'unica sessione MCP.
- `tests/integration/*.json` sono scenari per un progetto di prova creato in `%TEMP%`, in un'istanza TIA **separata e senza interfaccia**. Richiedono `TIACMD_DEV=1`, che abilita il tool nascosto `dev` (`add_device`, `eval`).

  ```powershell
  $env:TIACMD_DEV=1
  python tests/dev/mcpdrive.py tests/integration/00_create_test_project.json tests/integration/40_blocks_write.json
  ```

## Limiti della versione attuale

- Non ancora presenti: hardware, library, technology_objects e alarm_text, l'editor di rung LAD/FBD (`networks[].rungs`, `insert_rung`, …) ed export XLSX (sono disponibili CSV e XML).
- `live_data` (S7CommPlus) è escluso; RUN/STOP della CPU non è leggibile via Openness.
- Download e confronto online sono implementati, ma vanno verificati con un PLC o con PLCSIM avviato.
