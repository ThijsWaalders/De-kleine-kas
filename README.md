# ESP32-S3 Microgreens Climate Control System

[Dutch README/Manual](/READMEnl.md)

[English README/Manual](/READMEeng.md)

---

### Changelog Firmware v3.4.0

1. Architectuur & Centrale Beslisboom (ClimateLogic)

- **Centrale module-koppeling:** Alle klimaatlogica, veiligheidschecks en adviezen zijn volledig gecentraliseerd in ClimateLogic.cpp en ClimateLogic.h.
- **Vier-stage evaluatie**: De hoofdfunctie evaluateClimateState() is geoptimaliseerd met vier duidelijke argumenten om kas-, woning- en buitendata naadloos te synchroniseren.
- **Anti-hunt filters**: Er is een robuust anti-hunt filter toegevoegd om te voorkomen dat ventilatoren continu aan- en uitslaan bij grenswaarden (MIN_FAN_RUN_TIME en persistente checks).

2. Configuratie & Geheugenbeheer (Config)

- **Decoupling van globale variabelen**: Alle dubbele globale definities (zoals kasAdvice, kasAdviceReason, isClimateCrash, velocity-tracking en timers) zijn definitief opgeschoond en centraal ondergebracht in Config.cpp.
- **Linker-fouten opgelost**: Alle multiple definition en undefined reference compilatieproblemen zijn verholpen, wat resulteert in een 100% schone bouwstatus voor de ESP32-S3 Zero.

3. Telegram Bot & Interactie (Telegram)

- **Volledige v3.4.0 Handler**: De Telegram-module is volledig hersteld en gekoppeld aan de centrale v3.4.0 logica.
- **Statusrapporten & Advies**: Commando's zoals /st (Status) en /ad (Advies) halen nu direct actuele en eenduidige data uit de centrale beslisboom.
- **Testcommando's & Kalibratie**: Alle geavanceerde klimaattests (/tu, /tk, /tw), fansweeps en de handmatige/automatische kalibratiemodus (/cal) werken vlekkeloos en sturen nette statusberichten terug.

4. Fysische Bescherming & Sensoren

- **Klimaatreductie & Crash-detectie**: Automatische noodstop (isClimateCrash) bij kritieke hittepieken of snelle vochtigheidsschommelingen (checkClimateVelocity).
- **Dauwpunt- en VPD-sturing**: Nauwkeurige berekeningen voor ontvochtiging en ventilatie op basis van de interactie tussen de kas, de woningbuffer en de buitenlucht.

## v3.3.2

Wat is er in deze versie aangepast?

1. **Soft Handover (10 seconden overgang)**: Zodra externe ventilatie inschakelt, zakt de intFan gedurende 10 seconden gecontroleerd naar INT_FAN_MIN_PWM om een abrupte luchtdip of plotselinge tocht te voorkomen. Daarna dooft hij uit naar 0 om tegenstroom te vermijden.
2. **Harde ondergrens in rust (INT_FAN_MIN_PWM)**: In gesloten stand (GREENHOUSE_CIRCULATE_INTERNAL) of ruststand valt de intFan nooit meer uit naar 0; hij draait altijd op minimaal INT_FAN_MIN_PWM om stilstaande luchtlagen rond de SHT31 sensor (en kiemgroenten) te voorkomen.
3. **Failsafe opgeruimd:** De oude harde failsafe die de intFan direct genadeloos naar 0 trok, is verwijderd en vervangen door de gecontroleerde overgangslogica.
4. **Oude uitgecommentarieerde blokken verwijderd**: Alle oude codeblokken onderin zijn netjes opgeruimd.