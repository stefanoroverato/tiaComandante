# Stato del progetto

Ultimo aggiornamento: 2026-10-05.
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
  - "blast radius" (oggetti che dipendono dal blocco) per le modifiche alle interfacce;
  - `ExclusiveAccess` + transazione intorno alle azioni che modificano il progetto (un passo di annullamento in TIA, rollback se la chiamata fallisce o viene annullata), disattivabili con `exclusiveAccess`/`transactions` in config.json;
  - credenziali (UMAC, PLC) solo nel Gestore credenziali di Windows, inserite col dialogo di Windows e mai dagli argomenti dei tool.
- **Git:** submodule cJSON v1.7.19 e Mini-XML v4.0.6 sui fork `github.com/stefanoroverato/{cJSON,mxml}`. Repository pubblico `github.com/stefanoroverato/tiaComandante` (email autore noreply).

### Tool (17 tool, 213 azioni su 230 di TiaCommander)

| Tool | Azioni | Stato dei test |
|---|---|---|
| get_info | — | ✅ |
| session | 16/16 | ✅ connect/create/open/list/close/disconnect/configure/archive; save_as scritto ma non provato |
| admin | 13/13 + 3 | ✅ export store, statistiche, catalogo hardware locale e profili dei device; in più le 3 azioni delle credenziali |
| blocks_read | 15/17 | ✅ sul progetto reale (sola lettura) e su progetti di prova |
| blocks_write | 19/26 | ✅ su progetti di prova (scenario 40) |
| db | 11/11 | ✅ scenario 10 |
| udt | 11/11 | ✅ scenario 20 |
| tag | 16/16 | ✅ scenario 30 |
| folders | 5/5 | ✅ scenario 50 e progetto reale (lettura) |
| xref | 6/6 | ✅ sul progetto reale |
| watch | 14/14 | ✅ scenario 50 |
| diagnostics | 7/7 | ✅ con PLCSIM Advanced 8.0 (scenario 70) |
| hardware | 14/14 | ✅ scenario 90 (CPU 1511 con DI/DQ/AI, ET 200SP su PROFINET, tag di I/O); `export_xlsx` rimanda a CSV |
| library | 29/29 | ✅ scenario 91 (libreria globale di prova: copia di LStream V1.6 in `%TEMP%`, variabili `TC_LIB` e `TC_LIB_NAME`) |
| alarm_text | 15/15 | ✅ scenario 92 (liste di testi, voci, testi degli allarmi con `Program_Alarm`, classi di allarme) |
| technology_objects | 15/15 | ✅ scenario 93 (PID, assi, encoder: creazione, parametri, connessioni hardware, compilazione, export/import, master copy); `show_in_editor` richiede TIA con interfaccia |
| download_upload | 4/4 | ✅ check, download "solo modifiche" (trasferimento reale di un OB, risposta automatica a ConsistentBlocksDownload), upload_station in un progetto nuovo con `legacyCommunication=true` |

### Test eseguiti
- **CTest** `mcp_protocol`: handshake, schemi dei tool, errori JSON-RPC, chiusura pulita.
- **Scenari di integrazione** (`tests/integration/*.json`, driver `tests/dev/mcpdrive.py`, `TIACMD_DEV=1`): progetto di prova in `%TEMP%` in un'istanza TIA separata senza interfaccia, con una CPU 1511-1 PN V4.1.
- **Transazioni** (2026-10-02): scenari 00–50 rieseguiti con ExclusiveAccess + transazioni attive (129 chiamate, tutte come atteso dopo la gestione del commit rifiutato); scenario 80 verifica rollback (`dev eval failWith`) e commit, più save dentro ExclusiveAccess.
- **Progetto reale:** solo letture, più online/confronto/download "solo modifiche" verso un PLC simulato con PLCSIM Advanced 8.0 (IP impostato sul dispositivo). Scenari 70/71 con variabili `PLC_DEVICE`, `PLC_IP`, `PLC_PCIF` (es. `PLCSIM Virtual`).

## Da provare
- ✅ **upload_station** (2026-10-02): in un progetto vuoto l'upload dal PLCSIM riesce solo con `legacyCommunication=true` (`ConnectionConfiguration.EnableLegacyCommunication`). Senza questa opzione fallisce con "Online connection to PLC failed", perché il progetto non conosce ancora il PLC (comunicazione sicura PG/PC). Sono stati caricati hardware, gestione utenti, tag, tipi, blocchi e DB.
- ✅ `legacyCommunication` in `go_online` e `compare_online_offline` (2026-10-02, scenario 72 con PLCSIM: upload in un progetto nuovo, online, stato, offline, confronto "Identical"). Imposta `EnableLegacyCommunication` sulla `ConnectionConfiguration` del provider. Requisito lato CPU: comunicazione PG/PC legacy consentita.
- ✅ `legacyCommunication` in `download_to_device` (2026-10-05, PLCSIM). L'impostazione `EnableLegacyCommunication` è salvata nel progetto ed è condivisa tra la configurazione online e quella di download: il server la applica solo per la durata della chiamata e poi rimette il valore precedente.
- ✅ **Password dei PLC** (2026-10-05, PLCSIM con CPU 1511-1 PN V2.9 in protezione completa "NoAccess"): `go_online`, `compare_online_offline` e `download_to_device` entrano con la password salvata (`--credentials plc <IP>`, utente `-`), senza dialoghi. Online arriva `OnlineAuthenticationConfiguration` (il PLC dichiara solo `AnonymousUser` ma accetta `PasswordOnly`); il download chiede `ModuleWriteAccessPassword`; `upload_station` in un progetto nuovo (con `legacyCommunication=true`) chiede `ModuleReadAccessPassword`. Senza credenziale l'errore lo dice in modo esplicito. Dettagli: credenziali `plc/<IP>` (o `plc/*`) dal Gestore credenziali; risposte a `ModuleRead/WriteAccessPassword` (download e upload), `UploadPasswordConfiguration`, `OnlinePasswordConfiguration` e `OnlineAuthenticationConfiguration` (evento `ConnectionConfiguration.OnlineLegitimation`, utente PLC di progetto o globale; nome utente `-` = solo password). `BlockBindingPassword`, `PlcMasterSecretPassword` e la verifica dei certificati TLS non ricevono risposta e vengono riportati nell'output.
- ✅ **Policy `confirmations`** (2026-10-09, TIA con interfaccia): `cancel` risponde No ai dialoghi di conferma che arrivano durante una chiamata di un tool; fuori da una chiamata il dialogo resta all'utente di TIA, che lo vede e risponde. Il thread del worker era STA e TIA si bloccava al primo dialogo dell'interfaccia (vedi le note tecniche): ora è MTA. `accept` verificato allo stesso modo (cancellazione di una tabella delle variabili confermata con Yes, nessun dialogo); `ask` durante una chiamata lascia il dialogo a TIA come fuori dalle chiamate.
- ✅ **Worker MTA** (2026-10-09): regressione degli scenari 00–60, 80, 90–93 (281 chiamate) e PLCSIM riusciti. Con il PLCSIM (CPU 1511-1 PN V2.9 protetta, PLC `PLC_1`): upload della stazione con `ModuleReadAccessPassword`, online con `OnlineAuthenticationConfiguration`, confronto, download "solo modifiche" con `ModuleWriteAccessPassword`, `ConsistentBlocksDownload`, `AlarmTextLibrariesDownload` e `StartModules`, tutti con risposta automatica.
- ✅ Download con un blocco effettivamente modificato, senza STOP: OB caricato, richiesta `ConsistentBlocksDownload` gestita in automatico (2026-10-02).
- ✅ **Download che richiedono STOP** (2026-10-09, seconda istanza PLCSIM: CPU 1511-1 PN V2.9 nuova, comunicazione sicura): `hardware_software` e `hardware` con la CPU in RUN e `stopModules=true` (`StopModules` → `StopAll`, poi `StartModules`); con `stopModules=false` il download è annullato con "NOT ALLOWED - this download needs the CPU in STOP: repeat with stopModules=true". Cambio di struttura di un DB: con `reinitializeDataBlocks=false` annullato ("NOT ALLOWED - this download reinitializes data blocks…"), con `true` → `StopPlcAndReinitialize`. Confronto finale Identical. `OverwriteSystemData` → `Overwrite` verificato con un modulo aggiunto alla configurazione ("Delete and replace system data in target").
- ✅ **Comunicazione PG/PC sicura:** `trustPlcCertificate=true` (go_online, compare_online_offline, download_to_device) risponde `Trusted` alla verifica del certificato del PLC (`TlsVerificationConfiguration`) e riporta il messaggio di TIA; senza l'opzione la richiesta resta senza risposta e il messaggio indica l'opzione.
- [ ] Download e online su un PLC reale tramite una scheda di rete fisica.
- [ ] `go_online` senza `targetIp` su un progetto che ha l'IP nel progetto.
- [ ] `session save_as`, `launch` (`archive` provato nello scenario 90); `blocks_read export_all_xml`, `get_all_interfaces_summary` su progetti grandi (tempi).
- [ ] Registrazione e uso reale dei client: Claude Code (`claude mcp add`), Claude Desktop, Cursor, VS Code.
- [ ] Notifiche di progress e cancellazione con un client reale durante operazioni lunghe (compilazione, download).
- ✅ **Progetti protetti da UMAC** (2026-10-02): `session open` prova `Projects.Open(FileInfo)`; se il progetto è protetto, usa `Projects.Open(FileInfo, UmacDelegate)` con le credenziali salvate nel Gestore credenziali di Windows (`umac/<percorso>` o `umac/*`; `--credentials umac` oppure `admin action=set_credential`). Verificato con un progetto reale protetto (utente di progetto), in una TIA con interfaccia: nessun login manuale.
- [ ] UMAC con utente globale (UMC, `--global`) e con `OpenWithUpgrade`.
- [ ] Progetti multilingua: scelta della lingua per commenti e titoli.

## Da fare
1. **Azioni mancanti** rispetto a TiaCommander: solo quelle dell'editor LAD/FBD e di `live_data`:
   - `blocks_read`: `get_edit_capabilities`, `get_element_pins`;
   - `blocks_write`, editor di rung LAD/FBD: `insert_rung`, `update_rung`, `delete_rung`, `populate_network`, `update_network_element`, `split_network`, `delete_scl_statement`, `create_block networks[].rungs`;
   - `live_data` (11) resta escluso per scelta (S7CommPlus); `open_manager` non si applica (nessuna GUI).
2. ~~Export XLSX~~: fatto senza nuove dipendenze (zip di .NET Framework tramite il bridge, XML con Mini-XML): tag, watch/force, hardware, dati degli allarmi.
3. **Comando `--allowlist`** per registrare l'exe nell'AllowList di Openness (richiede privilegi di amministratore).
4. **Integrazione in CTest:** portare gli scenari di integrazione in CTest come test opzionali (oggi sono manuali).
5. **Certificati TLS dei PLC:** opzione esplicita per accettare il certificato di un PLC (`TlsVerificationConfiguration`), oggi lasciata a TIA.

## Note tecniche su Openness V21
- **Import SimaticML:** serve l'elemento `<Namespace/>` (software unit) nell'AttributeList di blocchi e UDT.
- **OB:** il numero va indicato in modo esplicito; il server sceglie il primo libero da 123.
- **Creazione:** solo FB e InstanceDB hanno API dirette; FC, OB, GlobalDB e UDT si creano importando XML generato da modello. SCL con codice passa da una sorgente esterna (`GenerateBlocksFromSource`).
- **Export:** gli oggetti appena importati o modificati vanno compilati prima di poterli esportare (il server lo fa e ritenta).
- **Watch e force table:** le voci si modificano solo via XML. Gli operandi simbolici stanno in `Name`, gli indirizzi assoluti in `Address`.
- **Connessione online:**
  - `ApplyConfiguration` accetta solo l'interfaccia target (lo slot);
  - con l'IP "impostato sul dispositivo" si usa `GoOnline(ConfigurationAddress)`;
  - dopo il primo collegamento TIA considera la connessione configurata;
  - un download con interfaccia esplicita lascia una connessione temporanea, che il go offline successivo cancella ("not configured"): dopo un download riuscito il server riapplica l'interfaccia usata (`ApplyConfiguration`, come `configure_connection`);
  - l'evento `ConnectionConfiguration.OnlineLegitimation` può scattare anche dopo che l'handler è stato rimosso (per esempio durante un download): il server usa un unico callback permanente che risponde solo mentre è in corso una chiamata online.
- **Hardware:**
  - i moduli compaiono direttamente in `Device.DeviceItems` (accanto a rack e CPU), non sotto il rack; il sottomodulo con lo stesso nome del modulo porta indirizzi e canali (il server lo accorpa al modulo);
  - `Address.Length` è in bit, `StartAddress` in byte; `Address.AddressControllers` restituisce oggetti `AddressController`, la CPU è `OwnedBy`;
  - `Device` e la CPU offrono `ICompilable` (hardware e software), i moduli di interfaccia no; una CPU S7-1500 V4.1 appena creata non compila senza la password per i dati di configurazione riservati;
  - `PnDeviceName` diventa scrivibile dopo `PnDeviceNameAutoGeneration=false`;
  - `HardwareCatalog.Find` impiega circa un minuto alla prima chiamata in ogni istanza di TIA (11743 voci in V21), poi è veloce: per questo c'è la copia locale (`dump_catalog`, `admin search_device_catalog`);
  - `CaxProvider` (assembly Step7) esporta AutomationML per un device o per il progetto, con un log su file;
  - cambio di modulo/firmware (`ChangeType`) ed eliminazione non chiedono conferme.
- **Librerie:**
  - non esiste un'API per creare un tipo da un blocco (solo `CreateFromDocuments`): i tipi arrivano da librerie esistenti; `PlcBlockComposition.CreateFrom(versione)` crea il blocco e copia tipo e dipendenze nella libreria di progetto;
  - `PlcSoftware` fa da `IUpdateProjectScope` e `IInstanceSearchScope` (il progetto no);
  - **`UpdateProject` cancella le istanze dei tipi che non sono usate** (blocchi o tipi di dato non richiamati), anche senza transazione: `update_project` confronta blocchi e tipi di dato prima e dopo ed elenca quello che TIA ha tolto;
  - `MasterCopyComposition.Create` rinomina da solo in caso di nome già presente (`Main_1`); `ContentDescriptions.ContentType` è un `System.Type`;
  - `LibraryTypeVersion.Export` funziona anche per i tipi di una libreria globale; `GetSupportedExportFormats` no;
  - `CleanUpLibrary` su una libreria globale può non togliere nulla anche se i tipi non hanno dipendenti: il server riporta i conteggi prima e dopo;
  - `Archive(dir, nome, modo)` usa il nome così com'è: senza estensione il file non ha estensione. Il server aggiunge `.zal21` (librerie) e `.zap21` (progetti) in modalità compressa.
- **Liste di testi e allarmi:**
  - Openness V21 non ha API per le voci delle liste di testi: esistono solo l'export/import Excel (`PlcAlarmTextListProvider.ExportToXlsx` / `ImportFromXlsx`). Le azioni sulle voci esportano la lista, modificano le righe e la reimportano con `ImportOptions.Override`, che sostituisce tutte le voci delle liste presenti nel file (così si cancellano anche le voci);
  - l'import di TIA è fragile: un XLSX con stringhe inline o con celle mancanti **fa terminare l'istanza di TIA**. Il modulo `tia/xlsx` scrive lo stesso formato degli export di TIA (shared strings, stili, proprietà `FileVersion`/`FileContent`); senza `FileVersion` l'import risponde "The version specified in the XLSX file is incorrect";
  - lo zip degli XLSX passa da `System.IO.Compression` di .NET Framework (`ZipFile`, `ZipFileExtensions.CreateEntryFromFile` per avere nomi con `/`): nessuna libreria esterna;
  - un PLC senza liste di testi non si può esportare ("There is no text list"); la prima lista si crea importando un file generato;
  - i testi degli allarmi (`PlcAlarmTextProvider`) esistono solo per le istanze (DB di istanza di un FB con `Program_Alarm`), altrimenti "There is no exportable alarm";
  - le classi di allarme (`AlarmClassDataProvider`, servizio del progetto) si esportano solo in un file `.dat` (uno zip con un XML `AlarmServiceGlobalSettingsData`); ogni altra estensione dà "invalid extension". L'import non è permesso dentro una transazione;
  - TIA scrive i log di import/export nella cartella `Logs` del progetto (`<LogEntry type><Message>`): il server ne riporta i messaggi.
- **Dialoghi di conferma (evento `Confirmation`):**
  - le operazioni di Openness non chiedono conferme: 27 operazioni provate (cancellazioni di tipi, tabelle, blocchi, moduli, sottoreti e sistemi IO usati; IP duplicato; cambio di firmware e di versione dell'HMI; rimozione di una lingua con testi; generazione da sorgente sopra un blocco esistente; chiusura con modifiche non salvate; sovrascrittura e cancellazione di un blocco aperto nell'editor con modifiche) vengono eseguite in silenzio o falliscono con un'eccezione;
  - quando un client è iscritto a `Confirmation`, TIA gli inoltra i dialoghi della propria interfaccia (es. "Do you really want to delete the selected objects?" quando l'utente cancella dall'albero del progetto) e aspetta la risposta;
  - Openness consegna gli eventi a un client STA solo mentre quel thread è dentro una chiamata di Openness: con il worker STA in attesa TIA restava bloccato senza mostrare il dialogo finché il server non faceva una chiamata (anche pompando i messaggi COM). Con il worker MTA gli eventi arrivano subito su un thread di Openness;
  - se il gestore non imposta `IsHandled`, TIA mostra il dialogo all'utente.
- **Oggetti tecnologici:**
  - `TechnologicalObjects.Create(nome, famiglia, Version)`: il bridge converte le stringhe in `System.Version`. TIA non elenca le versioni valide quando ne rifiuta una ("does not exist or is not a valid technology object"): le versioni accettate in V21 su S7-1500 sono in `capabilities` (assi ed encoder 7.0–10.0, PID_Compact 2.4/3.0, PID_3Step 2.3, PID_Temp 1.1/2.0, High_Speed_Counter 3.0–5.0, SSI 3.0);
  - la creazione accetta anche versioni non adatte al firmware della CPU: la compilazione le allinea (es. asse 7.0 → 10.0, contatore 5.0 → 4.0);
  - i parametri sono coppie Name/Value (210 per un asse di posizionamento); Openness non dice se un parametro è scrivibile;
  - `SensorInterface` è una lista per gli assi e un'interfaccia singola per l'encoder esterno; gli indirizzi non impostati valgono -1;
  - compilare la cartella degli oggetti tecnologici compila tutto il software del PLC; un oggetto non coerente non si esporta.
- **Download hardware e sicurezza della CPU:**
  - `download_check` con `mode=hardware|hardware_software` (e `download_to_device` in questi modi) compila tutto il dispositivo, configurazione hardware compresa, ed elenca gli errori: prima controllava solo il software e diceva READY anche quando il download hardware sarebbe fallito;
  - l'upload di una stazione non porta nel progetto la password dei dati di configurazione riservati (`PlcMasterSecretConfigurator`, `WithPassword`): la configurazione hardware non compila e il PLC rifiuta un download hardware con una password diversa ("The passwords for confidential PLC configuration data in the online PLC and in the project are not the same"). `Unprotect()` senza password fallisce ("PLC Master Secret is not provided"); `Reset()` porta a `WithoutPassword`, poi `Unprotect()` a `None`, ma il PLC rifiuta comunque il download;
  - una CPU creata con Openness non ha i valori della procedura guidata di TIA: V4.1 con `MasterSecretConfiguration=WithoutPassword` e nessun utente con "Full access" (errori di compilazione hardware; per le prove: `Unprotect()` e `PlcAccessControlConfiguration=Disabled`); V2.9 con un livello di accesso superiore a "Full access" senza password (per le prove: `PlcAccessLevelProvider.PlcProtectionAccessLevel=FullAccess`);
  - dopo il primo download hardware la CPU accetta solo la comunicazione sicura (impostazione predefinita di V21): `legacyCommunication=true` non si connette più e un `GoOnline` in legacy **fa terminare l'istanza di TIA senza interfaccia**; serve la comunicazione sicura con `trustPlcCertificate=true` se il progetto non conosce ancora il certificato del PLC;
  - dopo un `GoOnline` fallito il provider può restare online e TIA rifiuta compilazione, salvataggio e download ("not permitted in online mode"): `go_online` e `compare_online_offline` tornano offline in caso di errore e riportano l'errore originale.
- **Download verso PLCSIM:** un progetto creato con Openness non ha "Support simulation during block compilation" e il download fallisce con "'Main [OB1]' cannot be simulated". Si attiva con `PlcSimulationSettingsProvider.IsSimulationDuringBlockCompilationEnabled = true` (servizio del progetto).
- **Rinomina:** `PlcTag.Name`, `PlcWatchTable.Name` e il nome delle cartelle dei blocchi sono modificabili in V21.
- **AllowList:** identifica l'applicazione con percorso + SHA-256 dell'exe; nel registro sotto `HKLM\SOFTWARE\Siemens\Automation\Openness\AllowList`.
- **Transazioni:** `ExclusiveAccess.Transaction(project, text)`; `CommitOnDispose()` conferma, altrimenti `Dispose()` annulla tutto. `Compile` dentro una transazione fallisce ("The operation is not permitted within a transaction"): `session_compile()` conferma la transazione, compila e ne apre una nuova. TIA rifiuta anche il commit dopo un'eccezione avvenuta dentro la transazione ("Commit of a Transaction is not allowed after an exception is thrown"), anche se l'azione si è ripresa (es. watch: import → errore → compilazione → re-import). In quel caso la chiamata viene annullata e rieseguita senza transazione, purché non sia già stato confermato nulla. Save, download e upload sono esclusi (`AF_NO_TX`).
- **UMAC:** `Projects.Open(FileInfo, UmacDelegate)`; il delegate riceve `UmacCredentials` (Name, Type Project/Global, `SetPassword(SecureString)`). Il bridge converte le stringhe in `SecureString` e il testo della richiesta viene azzerato dopo l'uso.
