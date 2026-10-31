# Example: Waterkotte WWPR

The configuration mbproxy was developed with: a Waterkotte Ai1QLi air/water heat pump, WWPR controller on a
Carel pCO (firmware 01.05.04, BIOS 6.31), second-hand pCOWeb in "serial card 1" (BMS-Karte 1).

| | |
|---|---|
| Controller BMS menu | Kommunikation → BMS-Karte 1: protocol **Modbus Slave**, address **32** ("Speichern" on, otherwise the menu forgets it) |
| Serial | 19200 8N1 |
| Address space | HR 0–249, coils 0–199; anything beyond → exception 02. Analog `An` = HR n (×10), integer `In` = HR 128+n, digital `Dn` = coil n |
| Answer time | 20–240 ms (avg 107 ms) measured on the line |
| [commcfg](commcfg) | the card's pcod settings (Modbus Extended, limits) – only needed while pcod runs |
| [mbproxy.conf](mbproxy.conf) | mbproxy options incl. the write allowlist |

The register map and the Home Assistant integration (YAML package + dashboard) are in
[ha-waterkotte-wwpr](https://github.com/irockel/ha-waterkotte-wwpr).
