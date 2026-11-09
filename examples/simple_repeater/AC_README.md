# MeshCore Repeater – „ac“-Fork (Anti-Kollision)

Basis: MeshCore v1.18.0. Firmware-Version meldet sich als `v1.18.0-ac`.
Der Mesh-Kern ist unverändert, der Repeater bleibt voll kompatibel.
**Ab Werk ist alles aus** – nach dem Flashen verhält er sich wie das Original.

## Funktionen

| Befehl | Wirkung |
|---|---|
| `set dup.suppress N` | Hört der Repeater, während seine eigene Flood-Wiederholung noch in der Warteschlange steckt, N Kopien desselben Pakets von anderen, verwirft er seine eigene. `0` = aus, `1`–`5`. |
| `set txdelay.snr on` | Ordnet die Sendeverzögerung nach Empfangs-SNR: schwach empfangene Pakete früh, stark empfangene spät. Das Zeitfenster bleibt so groß wie bei `txdelay`. Wirkt nur auf weitergeleitete Floods. |
| `set advert.throttle MIN` | Flood-Adverts desselben Knotens werden höchstens einmal pro MIN Minuten weitergeleitet. `0` = aus. |

Anzeigen: `ac` (Einstellungen), `ac stats` (Zähler), `ac reset` (Zähler löschen),
`get dup.suppress`, `get txdelay.snr`, `get advert.throttle`.
`clear stats` setzt die Zähler ebenfalls zurück.

`ac stats` zeigt:
- `cancelled` – eigene Aussendungen, die dup.suppress eingespart hat
- `copies` – gehörte Kopien von Paketen, die noch in der Warteschlange lagen
- `adv_throttled` – nicht weitergeleitete Flood-Adverts
- `snr_sched` – nach SNR eingeplante Wiederholungen

Die Einstellungen liegen in `/fork_prefs`, getrennt von `/prefs.json`.
Ein Wechsel zurück zur Original-Firmware ist jederzeit möglich.

## Startprofil

**Ortsversorger (SEE, Berstadt)** – sollen still bleiben, wenn andere schon senden:
```
set txdelay.snr on
set dup.suppress 1
set advert.throttle 60
```

**Fernbrücke (Hochsitz)** – soll zuverlässig weiterleiten:
```
set txdelay.snr on
set advert.throttle 60
```
`dup.suppress` am Hochsitz aus lassen oder höchstens `2`, damit die Fernverbindung nicht wegfällt.

Wenn Nachrichten in Randgebieten schlechter ankommen: `dup.suppress` um 1 erhöhen oder ausschalten.

## Bauen

- **GitHub Actions:** Workflow „Build AC Repeater (RAK4631)“ läuft bei jedem Push auf `ac-fork`
  oder manuell; Ergebnis (`.uf2` + `.zip`) als Artifact.
- **Lokal:** `pio run -e RAK_4631_repeater`

## Flashen (RAK4631)

- **UF2:** per USB anschließen, Reset 2× schnell drücken → Laufwerk erscheint → `.uf2` draufziehen.
- **DFU/OTA:** `.zip` mit nRF Connect oder dem MeshCore-Flasher einspielen.

Einstellungen und Schlüssel bleiben beim Update erhalten.
