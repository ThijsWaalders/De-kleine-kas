📖 Handleiding: Robuuste Klimaatregeling & MQTT-architectuur (ESP32-S3 Kiemgroenten-systeem)

    📖 Handleiding: Robuuste Klimaatregeling & MQTT-architectuur (ESP32-S3 Kiemgroenten-systeem)

        LED Status

        1. Kerncomponenten van het Klimaatregelsysteem (ClimateLogic.cpp)

            Prioriteiten Volgorde

        2. Slimme Omgevingsbeoordeling & 3-Klimaten Keten (Kas vs. Woning vs. Buiten)

        3. Telemetrie, MQTT & Telegram-integratie

            Tips voor je Grafana / Home Assistant Dashboard op basis hiervan:

Deze handleiding beschrijft de opzet en werking van de klimaatsturing, de datastroom naar Home Assistant/Grafana en de proactieve Telegram-notificaties voor de ESP32-S3 kiemgroenten-kas. Het systeem is ontworpen om robuust, betrouwbaar en 'domme-sensor-bestendig' te zijn.
LED Status

Door prioriteiten te stellen (kritieke meldingen boven normaal gedrag), weet de LED altijd feilloos wat te tonen.

Hier is de logische volgorde van belang (van hoog naar laag):

    Blauw = Netwerk / Wi-Fi actueel of verbinden / AP-modus.

    Geel = Extern weer-alarm / waarschuwing (aankomend ruig of koud weer buiten).

    Oranje = Actie vereist van JOU (als woning-actuator, bijv. binnendeur op een kier zetten of de woningventilator aanzetten).

    Groen = ESP regelt de kas zelfstandig / alles is optimaal.

    Uit = Systeem in diepe rust.

1. Kerncomponenten van het Klimaatregelsysteem (ClimateLogic.cpp)

Het klimaatregelsysteem is opgedeeld in een aantal slimme, zelfstandige lagen:
A. Filter voor Goedkope Sensoren (Voortschrijdend Gemiddelde)

Goedkope sensoren (zoals de DHT22 of vergelijkbare modules) hebben vaak last van meetruis, kleine sprongen en uitschieters.

    Hoe het werkt: De software houdt een buffer bij van de laatste 5 minuten (SENSOR_BUFFER_SIZE = 5). Elke minuut wordt er een sample toegevoegd.

    Het voordeel: Het systeem reageert nooit op een willekeurige meetfout van nu, maar kijkt naar een stabiel gemiddelde over tijd. Dit voorkomt dat actuatoren onnodig gaan klapperen.

B. Vaste, Strenge VPD-grenzen (Geen Verslonsing)

VPD (Vapor Pressure Deficit) is cruciaal voor kiemgroenten; bij een te hoge VPD drogen ze uit, bij een te lage VPD ontstaat er schimmel.

    Hoe het werkt: Er is bewust gekozen om geen adaptieve of zelflerende offsets op de VPD te gebruiken. Het systeem houdt een harde, consistente norm aan zodat je tijgerstrakke en eerlijke data in Grafana ziet.

C. Anti-Hunt Beveiliging voor Ventilatoren

    Hoe het werkt: De functie applyAntiHuntFilter dwingt een minimale looptijd af (MIN_FAN_RUN_TIME = 180000 ms / 3 minuten).

    Het voordeel: De hardware wordt ontlast en het binnenklimaat krijgt de tijd om rustig te stabiliseren.

D. Proactieve Snelheidsbewaking (Velocity Check)

    Hoe het werkt: Het systeem kijkt niet alleen naar de absolute waarden, maar ook naar de stijgingssnelheid van de luchtvochtigheid (humidityVelocity). Bij plotselinge pieken grijpt het systeem in met interne circulatie.

Prioriteiten Volgorde (Hiërarchisch)

De volgorde van prioriteiten is waterdicht vastgelegd:

    Noodtoestand & Kritiek heet: Als de kas uit de hand loopt (> 28°C), grijpt het systeem direct in met maximale geforceerde ventilatie om oververhitting te voorkomen.

    Buffer-bewustzijn (Anticipatie): Als er een koude weersomslag buiten aankomt, maar de woningbuffer is stabiel en warm genoeg, houdt het systeem de kas gesloten om de warmte vast te houden.

    Schimmel-anticipatie: Als de kweeklampen uitgaan en de vochtigheid stijgt, start het systeem preventief interne circulatie tegen condens op de kiemgroentes.

    Te droog / Uitdrogingsrisico (Hoge VPD): Als de kas door de lampen of omgevingswarmte te droog dreigt te worden, krijgt vochtbehoud prioriteit en blijft de kas hermetisch gesloten (OFF).

    Te klam / Schimmel- of Condensrisico: Pas hierna kijkt de ESP of de woninglucht (via binnendeur/woningventilator) of de buitenlucht (via buitenraam) het meest geschikt is om te ontvochtigen, of start interne circulatie.

    Te warm (Normale overschrijding): Koelen met de slim gekozen buiten- of binnenlucht.

    Te koud: Circuleren met warmte uit de woning.

    Balans: Alles in orde.

2. Slimme Omgevingsbeoordeling & 3-Klimaten Keten (Kas vs. Woning vs. Buiten)

Bij het bepalen van het ventilatieadvies kijkt de software in een keten van Buiten ➔ Woning ➔ Kas:

    Woningbuffer: De ESP beoordeelt of de woning stabiel en droger is dan de kas (indoorIsViable), zodat je met de binnendeur en woningventilator een veilige luchtstroom creëert.

    Buitenlucht: De software controleert of de buitenlucht koeler en niet te klam is (outHum < 85.0), zodat je geen mist of klamme buitenlucht naar binnen trekt.

3. Telemetrie, MQTT & Telegram-integratie

Om alle data netjes te loggen in Grafana, te bewaken in Home Assistant én jou direct op de telefoon te informeren, stuurt de ESP32-S3 telemetrie en actie-adviezen uit.
Proactieve Telegram-notificaties met Actie-instructies

Zodra de status wijzigt, stuurt de ESP een glashelder bericht naar je telefoon waarin exact staat welke fysieke handeling je moet verrichten:

    Woninglucht inzetten: "Zet de binnendeur op een kier en zet de woningventilator aan."

    Buitenlucht inzetten: "Zet het buitenraam open."

    Systeem in balans: Bevestiging dat het kiemklimaat optimaal behouden blijft.