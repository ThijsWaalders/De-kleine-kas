# To-Do List

- [To-Do List](#to-do-list)
  - [Fix / Choose the Right Wi-Fi Connection to get OTA's to work](#fix--choose-the-right-wi-fi-connection-to-get-otas-to-work)
  - [I Did](#i-did)
    - [ClimateLogic](#climatelogic)
    - [Display](#display)

## Fix / Choose the Right Wi-Fi Connection to get OTA's to work

Lekker Engderland praten..

## I Did

### ClimateLogic

Wat er is aangepast op basis van je feedback:

    Adaptieve VPD-offset is volledig verwijderd: We gebruiken nu keiharde, vaste en betrouwbare onder- en bovengrenzen voor de VPD (VPD_MIN_OPTIMAL en VPD_MAX_OPTIMAL), zodat het systeem nooit zijn eigen standaarden verlaagt ("lui" wordt).

    Voortschrijdend gemiddelde (Moving Average / Buffer) voor sensoren: Omdat goedkope sensoren (zoals DHT of vergelijkbaar) last hebben van ruis, pieken en sprongen, worden temperatuur, luchtvochtigheid en VPD nu gebufferd over een langere periode (bijv. de laatste 3 à 5 minuten). Het systeem reageert dus pas op een structurele trend in plaats van op een willekeurige meetfout van NU.

    Indoor- en Outdoor-VPD / Dauwpunt evaluatie: Er is nu ook logica opgenomen die kijkt naar de buiten- en binnenlucht. Zo kan de logica beoordelen of buitenlucht of kamerlucht daadwerkelijk een verbetering oplevert (bijv. voorkomen dat je kurkdroge of klamme buitenlucht de kas inblaast).

### Display

Wat zie je dan op je display?

    Als de kas niet kan luchten (handrem erop): De gele bovenbalk roept ACTIE: Huis luchten! en de onderste regel toont netjes Intern: 41% (of wat de basissnelheid/bijgeregelde waarde ook is). Zo zie je direct dat de interne fan wel zijn werk blijft doen om de lucht in beweging te houden, maar dat de externe afvoer wacht op een drogere kamer.

    Als de kas wel kan luchten: De onderste regel springt over naar Afvoeren (XX%).

Zo blijft de code lekker compact, puur leunend op de variabelen die er al zijn (fanExt1Speed en fanIntSpeed). Past dit precies op je schermregels?