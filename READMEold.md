# README

## OTA

Over The Air updates going TO the ESP on port 3232

## Telnet

Is used to listen (live debugging) and using port 23



## Systeemoverzicht & Architectuur

Dit systeem regelt het microklimaat in 'De Kleine Kas' via een ESP32-S3 microcontroller. Het bewaakt continu de temperatuur, luchtvochtigheid, luchtdruk en lichtsterkte. Op basis hiervan worden twee externe ventilatoren, een interne circulatieventilator en een verwarmingsmat automatisch aangestuurd, met volledige bediening en rapportage via Telegram en MQTT.
Belangrijke Instellingen (Config.h)

1.  VPD-drempels (VPD_MIN_OPTIMAL / VPD_MAX_OPTIMAL): Bepalen de ideale comfortzone voor planttranspiratie.
2.  Schimmelgrens (HUM_MOLD_THRESHOLD): De luchtvochtigheidswaarde waarboven het schimmelrisico wordt geactiveerd.
3.  Temperatuurgrenzen mat (HEAT_MAT_TEMP_LOW / HIGH): Bepalen wanneer de verwarmingsmat automatisch in- of uitschakelt.
4.  Anti-hunt tijd (MIN_FAN_RUN_TIME): Standaard ingesteld op 3 minuten om te voorkomen dat ventilatoren onrustig gaan "dansen" door windfluctuaties.

### Het Zelflerende Systeem (Adaptieve VPD)

    Het systeem leert autonoom van historisch gedrag via de functie updateSelfLearningParameters.

    Wanneer er langdurig geen schimmelrisico optreedt, past de software de adaptieve VPD-ondergrens geleidelijk aan (met kleine stappen van -0.02 kPa tot maximaal -0.2 kPa). Hierdoor optimaliseert het systeem zichzelf op basis van de werkelijke omstandigheden in jouw ruimte.

### Proactieve Bewaking & Snelheid

    Vochtigheidssnelheid (checkClimateVelocity): Berekent of de luchtvochtigheid plotseling extreem snel stijgt (bijv. ≥+3%/min bij een LV >60%). Bij een dreigende piek grijpt de software direct in met preventieve circulatie voordat een kritisch niveau wordt bereikt.

    Trendanalyse & Extremen: Barometrische trends en dagelijkse hoogste/laagste waarden worden automatisch bijgehouden voor diepgaande diagnoses via Telegram-commando's zoals /status, /advies en /minmax.

### Hoe zie je wat het systeem doet? (Transparantie & Diagnostiek)

Het systeem is zo gebouwd dat het al zijn stappen luid en duidelijk logt. Je kunt op twee plekken meekijken:

#### Via Telegram (De Snelle Check)

- Stuur /advies (/ad): Dit is je belangrijkste diagnose-tool. Hier zie je direct de schimmelreden, de exacte VPD-waardes, de dauwpunt-marge en de reden waarom de ventilatoren of de mat aanstaan.
- Stuur /status (/st): Toont de actuele sensorwaarden en de algehele status van de regeling.
- Stuur /hwcheck (/hc): Toont de ventilatorsnelheden in zowel percentages (%) als echte rotaties per minuut (RPM), plus sensorfouten en vrij geheugen.

#### Via de Serial Monitor / Syslog (De Diepe Duik):

 - Als je verbonden bent met de seriële poort zie je tags zoals [ADVICE], [ANTI-HUNT], [AI LEARNING] en [ALERT]. Als de ventilator van status verandert, zie je daar direct de logische reden bij staan (bijv. "Kas overschrijdt max temp en kamer is koeler; afvoeren").

### Hoe werkt de besluitvorming? (Waarom draait een fan zo hard?)

Het systeem werkt met een prioriteitenmatrix in ClimateLogic.cpp. Het beoordeelt het klimaat cyclisch op basis van de volgende regels:

1. Oververhitting (Prioriteit 1): Als de kas heter wordt dan de max temperatuur (GREENHOUSE_MAX_TEMP), kijkt het systeem of de woning koeler is. Zo ja, dan gaat externe ventilator 1 vol aan om af te voeren naar de woning. Is de woning warmer? Dan kijkt hij of er schimmelrisico is; zo ja, toch afvoeren, zo nee, dan gaat de interne ventilator draaien om de hitte te breken.
2. Vocht & Schimmel / VPD (Prioriteit 2): Als de lucht te klam is (VPD te laag) of de luchtvochtigheid boven de drempel (HUM_MOLD_THRESHOLD) komt, grijpt het systeem in. Het berekent of ventileren naar de kamer helpt (op basis van het dauwpunt van de kamer).
3. Anti-Beveiliging (Anti-Hunt): Om te voorkomen dat ventilatoren bij elke windvlaag of kleine sensorfluctuatie als een gek op- en neergaan ("dansen"), zit er een timer in (MIN_FAN_RUN_TIME = 3 minuten). Als een ventilator aangaat, blijft hij minimaal 3 minuten in die stand draaien, tenzij een noodsituatie (zoals een extreme vochtpiek) ingrijpt.
4. Aansturing van de Fans: De interne en externe ventilatoren worden aangestuurd via PWM (waarden van 0 tot 255), die in de rapportages netjes worden omgezet naar percentages (0% tot 100%) en gekoppeld aan echte RPM-metingen als je tacho-pennen hebt aangesloten.

#### Wat kun je zelf bijstellen in Config.h

Als je merkt dat het systeem net iets te fanatiek of juist te laks reageert, kun je de volgende parameters in Config.h fijnregelen:

1. VPD_MIN_OPTIMAL & VPD_MAX_OPTIMAL: De ideale onder- en bovengrens voor de planttranspiratie (VPD in kPa). Verhoog je de ondergrens, dan gaat het systeem sneller ventileren tegen te klamme lucht.
2. HUM_MOLD_THRESHOLD: Standaard vaak rond de 75-80%. Als jouw planten gevoelig zijn voor schimmel, zet je deze lager (bijv. 72%).
3. HEAT_MAT_TEMP_LOW / HIGH: De temperatuurgrenzen waarop de verwarmingsmat aan- en uitslaat.
4. MIN_FAN_RUN_TIME: Wil je dat de ventilatoren sneller reageren op wisselingen? Dan kun je deze verlagen van 180000 ms (3 minuten) naar bijvoorbeeld 60000 ms (1 minuut).
5. De Toekomst: Sturen op basis van Weer-API (Predictief Klimaat)

#### Zo werkt zo'n predictieve (voorspellende) laag in de praktijk:

1. Reactief vs. Predictief: Nu reageert het systeem op wat er nu gebeurt (bijv. de kas wordt vochtig, dus we gaan nu ventileren). Met een weer-API (zoals OpenWeatherMap) weet de ESP32 wat er over 2 uur aan komt (bijv. "Er komt een zware bui aan met een luchtvochtigheid van 95% en de temperatuur duikelt omlaag").

W#### at het systeem dan kan doen:

1.  Als de API meldt dat het over een uur flink opwarmt door zonneschijn, kan de kas nu alvast een tikje extra geventileerd worden om temperatuurpeaks voor te zijn.
2.  Als er buiten zware regen op komst is (waardoor de buitenlucht te nat is om te ventileren), kan het systeem besluiten om de luiken/ventilatoren naar buiten gesloten te houden en puur op de binnenlucht of de warmtemat te vertrouwen.