# To-Do List

- [To-Do List](#to-do-list)
  - [Voor dev board](#voor-dev-board)
  - [Code](#code)
    - [Sensor Offsets](#sensor-offsets)
  - [v3.4](#v34)
  - [v3.4.1](#v341)
    - [De 4 Pijlers van de Slimme Lampen-optimalisatie (v3.4.2)](#de-4-pijlers-van-de-slimme-lampen-optimalisatie-v342)

## Voor dev board

splits pwm en tacho gpio's zo ver mogelijk, kijk eerst zonder (met alleen wifi/ota update bijv. of weather api/telegram) overige code hoeveel warmte waar op de chip vandaan komt.

## Code

### Sensor Offsets

LV binnen wat iets hoger dan kas, offset lager zetten volgend kalibratie moment

## v3.4

## v3.4.1 

📦 Overzicht v3.4.1 — Dynamische Interne Ventilatie & Slimme Veiligheid
1. Vernieuwde updateFanSpeeds() in HardwareControl.cpp

De werkelijke hardware-aansturing voor de ventilatoren is uitgebreid met een dynamische opschaling van de interne ventilator (intFan). Zodra de kas gesloten is (GREENHOUSE_CIRCULATE_INTERNAL), blijft de fan niet meer passief op de minimale basissnelheid INT_FAN_BASE_PWM staan, maar schaalt hij traploos op tot FAN_MAX_PWM op basis van de VPD-afwijking ten opzichte van pidSetPointVPD en de status van de lampen.

2. Wat is er veranderd t.o.v. v3.3.1?

    Geen Stilstaande Warmtebel meer rond SHT31:

        In v3.3.1 bleef de intFan vaststaan op INT_FAN_BASE_PWM (111 PWM, ~1600 RPM) zolang de kas gesloten was.

        In v3.4.1 reageert hij direct zodra de lampen aanspringen of zodra het klimaat afwijkt van de pidSetPointVPD (0.70 kPa). Hij loopt dan soepel op richting FAN_MAX_PWM (255 PWM, ~4000 RPM) om de lucht krachtig langs het schild van de SHT31 te trekken.

    Gecentreerde Configuratiewaardes:

        Geen 'magic numbers' in de logica; alles maakt direct gebruik van de constanten uit Config.h:

            INT_FAN_BASE_PWM (startwaarde / ruststand)

            FAN_MAX_PWM (maximale bovengrens)

            pidSetPointVPD (doelwaarde VPD)

            TEMP_HYSTERESIS_THRESHOLD (drempel voor activatie)

    Instandhouding Veiligheids Failsafe:

        De Failsafe-regel blijft 100% intact: zodra externe ventilatie (GREENHOUSE_VENTILATE) activeert, wordt de intFan direct uitgezet (targetIntSpeed = 0) om tegenstroom en valse luchtdruk te voorkomen.



### De 4 Pijlers van de Slimme Lampen-optimalisatie (v3.4.2)

1. **Energie- en Kostenoptimalisatie (Daluren & Prijzen)**

- **Het idee**: In Nederland (en met dynamische contracten of dal/piek tarieven) is stroom 's nachts of in de ochtend vaak aanzienlijk goedkoper.

- **De integratie**: De ESP kan (via een lichte API of lokale tijdregels) weten wanneer de daluren/goedkope uren vallen.

- **Het doel**: De 12 uur belichting bij voorkeur inplannen tijdens deze goedkopere uren (bijv. van 23:00 tot 11:00 uur of van 22:00 tot 10:00 uur), maar.

1. **Thermische Balans & "Minste Werk" (Minimale Actuator-belasting)**

- **Wat betekent 'minste werk'?**
  - Als de lampen aangaan op het moment dat de buitentemperatuur of woningtemperatuur toch al stijgt (bijv. middagzon), moet de kas enorm hard koelen en ventileren om de hitte kwijt te raken. Dat kost energie (fans) en brengt risico's op uitdroging met zich mee.

  - Als de lampen daarentegen branden in een periode dat de omgeving van nature koeler is, gebruikt de kas de warmte van de lampen juist als "gratis verwarming" (waardoor de warmtemat minder aan hoeft), en kan de restwarmte na uitschakeling geleidelijk wegvloeien.

- **Het leeraspect**: De ESP houdt historisch bij op welke tijdstippen de ventilatoren (Fan Int/Ext) en de warmtemat het minste hoefden in te grijpen tijdens de 12-uurs cyclus. De tijdswindow waarin het systeem het stabielst draait met de minste correcties, wordt door het algoritme beloond als de "beste" cyclus.

3. **Seizoensgebonden Adaptatie (Zomer vs. Winter)**

    De uitdaging: Wat in de winter een geweldige cyclus is (lampen 's nachts aan om warmte te leveren tegen de kou), is in de hete zomer een ramp (want dan wil je de warmte van de lampen juist kwijt in de koelste uren van de nacht, en wil je de piekelders van de dag vermijden).

    Het seizoensprofiel: Het algoritme mag niet statisch zijn, maar moet kijken naar de gemiddelde buitentemperatuur van de afgelopen 7 dagen.

        Winter: Lampen aan tijdens de koudste uren (nacht/ochtend) om te verwarmen.

        Zomer: Lampen aan tijdens de koelste uren van de nacht, en overdag (wanneer het buiten al warm is) een 'donkere rustfase' inlassen om hitteschade en extreme VPD-pieken te voorkomen.

2. De Vraagbaak: Het Telegram-commando (bijv. /advieslampen of /plan)

Je wilt niet dat het systeem volledig achter je rug om ineens de schema's omgooit zonder dat je weet waarom. Je wilt er inzicht in hebben.

    Als je typt /plan of /lampadvies, berekent en antwoordt de bot straks zoiets:

        🧠 AI-Lampadvies voor morgen (Seizoen: Herfst)
        • Aanbevolen window: 23:00 tot 11:00 uur (12 uur).
        • Waarom:

            📉 Energie: Volledig in de goedkope daluren (laagste stroomprijs).

            🌡️ Thermisch: Buitentemperatuur is laagste; restwarmte van lampen voorkomt dat de warmtemat 's nachts aanslaat.

            🔕 Minste werk: Verwachte ventilator-activiteit is minimaal (< 5% van de tijd).

Hoe gaan we dit bouwen in v3.4? (De techniek achter de schermen)

    Een "Score-algoritme" (Cost Function):
    Elke dag kent de ESP de totale "stress" van die dag (hoe vaak moesten de fans loeien? Hoe lang stond de warmtemat aan? Hoe vaak raakte de VPD uit balans?). De ESP koppelt die stress-score aan het gekozen tijdsblok van de lampen. Wat historisch de minste stress opleverde bij het huidige buitentemperatuurgemiddelde, wordt de winnende formule.

    Npreferences / SPIFFS opslag:
    De geleerde parameters slaan we netjes op in de Flash-geheugen (via Preferences), zodat de kas na een reboot of OTA-update niet opnieuw hoeft te beginnen met leren.

    Flexibele sturing:
    De lampen krijgen in v3.4 geen domme vaste timer meer, maar een dynamische starttijd die dagelijks (of perseizoen) subtiel bijstuurt op basis van het geleerde optimale profiel.

Hoe klinkt deze richting voor v3.4? Dit maakt de kas echt autonoom en slim op het gebied van energiebesparing en microklimaat-stabiliteit! 🌱🤖