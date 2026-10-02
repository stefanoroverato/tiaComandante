# Stato del progetto

Ultimo aggiornamento: 2026-10-02.
Riferimenti: piano iniziale (milestone M0–M4), README per build e uso, `third_party/README.md` per le librerie.

## Fatto

### Infrastruttura
- **Build:** CMake + MSVC x64, CRT statica. Il bridge .NET si compila con il `csc.exe` di .NET Framework 4.8.
- **Composizione:** launcher `tiacomandante.exe` stabile (build deterministica, così l'approvazione nell'AllowList di Openness resta valida) + `tiacomandante-core.dll` + `TiaComandante.Bridge.dll`.
- **Bridge .NET generico:** handle, get/set/call con conversione dei tipi guidata dalla firma, `GetService<T>`, eventi e delegate richiamati in C, eccezioni complete.
- **Server MCP su stdio:** thread lettore + worker STA; negoziazione delle versioni 2024-11-05 … 2025-11-25; progress e cancel.
- **Supporto:** configurazione, export store (exportId, 24 h), statistiche ed errori recenti, log in `%LOCALAPPDATA%\tiaComandante\logs`.
- **Sicurezza:**
  - le azioni di scrittura non si collegano mai in automatico a un TIA in esecuzione;
  - modalità sola lettura;
  - frasi di conferma per le operazioni distruttive;
  - "blast radius" (oggetti che dipendono dal blocco) per le modifiche alle interfacce.
- **Git:** submodule cJSON v1.7.19 e Mini-XML v4.0.6 sui fork `github.com/stefanoroverato/{cJSON,mxml}`. Repository pubblico `github.com/stefanoroverato/tiaComandante` (email autore noreply).

### Tool (13 tool, 134 azioni su 230 di TiaCommander)

| Tool | Azioni | Stato dei test |
|---|---|---|
| get_info | — | ✅ |
| session | 16/16 | ✅ connect/create/open/list/close/disconnect/configure; archive e save_as scritti ma non provati |
| admin | 10/13 | ✅ export store, statistiche; mancano le 3 azioni del catalogo hardware |
| blocks_read | 15/17 | ✅ sul progetto reale (sola lettura) e su progetti di prova |
| blocks_write | 19/26 | ✅ su progetti di prova (scenario 40) |
| db | 11/11 | ✅ scenario 10 |
| udt | 11/11 | ✅ scenario 20 |
| tag | 16/16 | ✅ scenario 30 |
| folders | 5/5 | ✅ scenario 50 e progetto reale (lettura) |
| xref | 6/6 | ✅ sul progetto reale |
| watch | 14/14 | ✅ scenario 50 |
| diagnostics | 7/7 | ✅ con PLCSIM Advanced 8.0 (scenario 70) |
| download_upload | 4/4 | ✅ check, download "solo modifiche" (trasferimento reale di un OB, risposta automatica a ConsistentBlocksDownload), upload_station in un progetto nuovo con `legacyCommunication=true` |

### Test eseguiti
- **CTest** `mcp_protocol`: handshake, schemi dei tool, errori JSON-RPC, chiusura pulita.
- **Scenari di integrazione** (`tests/integration/*.json`, driver `tests/dev/mcpdrive.py`, `TIACMD_DEV=1`): progetto di prova in `%TEMP%` in un'istanza TIA separata senza interfaccia, con una CPU 1511-1 PN V4.1.
- **Progetto reale:** solo letture, più online/confronto/download "solo modifiche" verso un PLC simulato con PLCSIM Advanced 8.0 (IP impostato sul dispositivo). Scenari 70/71 con variabili `PLC_DEVICE`, `PLC_IP`, `PLC_PCIF` (es. `PLCSIM Virtual`).

## Da provare
- [x] **upload_station** (2026-10-02): in un progetto vuoto l'upload dal PLCSIM riesce solo con `legacyCommunication=true` (`ConnectionConfiguration.EnableLegacyCommunication`). Senza questa opzione fallisce con "Online connection to PLC failed", perché il progetto non conosce ancora il PLC (comunicazione sicura PG/PC). Sono stati caricati hardware, gestione utenti, tag, tipi, blocchi e DB.
- [ ] Valutare `legacyCommunication` anche per `go_online` e il download verso PLC non ancora presenti nel progetto, e documentare il requisito lato CPU (comunicazione legacy consentita).
- [x] Download con un blocco effettivamente modificato, senza STOP: OB caricato, richiesta `ConsistentBlocksDownload` gestita in automatico (2026-10-02).
- [ ] Download con modifiche che richiedono STOP: verificare la policy dei delegate (StopModules, StartModules, DataBlockReinitialization) e il messaggio quando una richiesta non viene gestita. Possibile opzione "rispondi a tutto" come in pyTia (prima scelta diversa da NoAction).
- [ ] Download `hardware` / `hardware_software` (OverwriteSystemData).
- [ ] Download e online su un PLC reale tramite una scheda di rete fisica.
- [ ] `go_online` senza `targetIp` su un progetto che ha l'IP nel progetto.
- [ ] `session archive`, `save_as`, `launch`; `blocks_read export_all_xml`, `get_all_interfaces_summary` su progetti grandi (tempi).
- [ ] Registrazione e uso reale dei client: Claude Code (`claude mcp add`), Claude Desktop, Cursor, VS Code.
- [ ] Notifiche di progress e cancellazione con un client reale durante operazioni lunghe (compilazione, download).
- [ ] Progetti protetti da UMAC: `session open` fallisce ("The project is protected. You are not authorized to open the project in write mode"). Per ora si apre il progetto dall'interfaccia di TIA, si entra con le credenziali e poi si usa `session connect`. Da implementare: `Projects.Open(FileInfo, UmacDelegate)` (`UmacCredentials`: Name, Type = Project/Global, SetPassword(SecureString)), con la password presa da variabile d'ambiente o da un prompt, mai dagli argomenti del tool.
- [ ] Progetti multilingua: scelta della lingua per commenti e titoli.

## Da fare
1. **Tool e azioni mancanti** rispetto a TiaCommander (96 azioni):
   - `hardware` (14), `library` (29), `technology_objects` (15), `alarm_text` (15);
   - `admin`: catalogo device (3);
   - `blocks_read`: `get_edit_capabilities`, `get_element_pins`;
   - `blocks_write`, editor di rung LAD/FBD: `insert_rung`, `update_rung`, `delete_rung`, `populate_network`, `update_network_element`, `split_network`, `delete_scl_statement`, `create_block networks[].rungs`;
   - `live_data` (11) resta escluso per scelta (S7CommPlus); `open_manager` non si applica (nessuna GUI).
2. **Export XLSX:** serve una libreria zip; proposta miniz (MIT) come fork, da discutere.
3. **Policy per l'evento Confirmation di TIA:** oggi i dialoghi vengono solo registrati e lasciati a TIA.
4. **`ExclusiveAccess` + `Transaction`** per le azioni di scrittura (un solo passo di annullamento in TIA).
5. **Password:** upload e download verso PLC protetti (DownloadPasswordConfiguration, password di upload).
6. **Comando `--allowlist`** per registrare l'exe nell'AllowList di Openness (richiede privilegi di amministratore).
7. **Integrazione in CTest:** portare gli scenari di integrazione in CTest come test opzionali (oggi sono manuali).

## Note tecniche su Openness V21
- **Import SimaticML:** serve l'elemento `<Namespace/>` (software unit) nell'AttributeList di blocchi e UDT.
- **OB:** il numero va indicato in modo esplicito; il server sceglie il primo libero da 123.
- **Creazione:** solo FB e InstanceDB hanno API dirette; FC, OB, GlobalDB e UDT si creano importando XML generato da modello. SCL con codice passa da una sorgente esterna (`GenerateBlocksFromSource`).
- **Export:** gli oggetti appena importati o modificati vanno compilati prima di poterli esportare (il server lo fa e ritenta).
- **Watch e force table:** le voci si modificano solo via XML. Gli operandi simbolici stanno in `Name`, gli indirizzi assoluti in `Address`.
- **Connessione online:**
  - `ApplyConfiguration` accetta solo l'interfaccia target (lo slot);
  - con l'IP "impostato sul dispositivo" si usa `GoOnline(ConfigurationAddress)`;
  - dopo il primo collegamento TIA considera la connessione configurata.
- **Rinomina:** `PlcTag.Name`, `PlcWatchTable.Name` e il nome delle cartelle dei blocchi sono modificabili in V21.
- **AllowList:** identifica l'applicazione con percorso + SHA-256 dell'exe; nel registro sotto `HKLM\SOFTWARE\Siemens\Automation\Openness\AllowList`.
