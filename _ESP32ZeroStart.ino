#include "config.h"
#include "LedManager.h"
#include "NetworkManager.h"

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // LED aanzetten en blauw maken tijdens opstarten
  statusLed.begin();
  statusLed.setColor(0, 0, 255); 

  // Netwerk starten (gebruikt automatisch alles uit config.h)
  setupNetwork();

  // Als we hier zijn is alles klaar -> LED groen
  statusLed.setColor(0, 255, 0);
}

void loop() {
  // Altijd als eerste aanroepen voor OTA en draadloze logs
  handleNetwork(); 
  
  // Hier start jouw eigen unieke projectlogica!
  static unsigned long lastMsg = 0;
  if (millis() - lastMsg > 10000) {
    lastMsg = millis();
    logPrintln(F("[App] Systeem draait stabiel..."));
  }
}