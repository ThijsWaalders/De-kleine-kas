/**
 * @file DecisionEngine.h
 * @brief Het besluitvormende brein voor De Kleine Kas.
 *        Beheert de autonome hiërarchie (Interne Autonomie -> Woningbuffer -> Externe Ventilatie).
 */

#ifndef DECISION_ENGINE_H
#define DECISION_ENGINE_H

#include "Config.h"

/**
 * @brief Evalueert alle sensordata en bepaalt autonoom de ventilatorstatus (kasAdvice).
 * @param inTemp Kas temperatuur
 * @param inHum Kas luchtvochtigheid
 * @param inDp Kas dauwpunt
 * @param outTemp Buiten/woning temperatuur
 * @param outHum Buiten/woning luchtvochtigheid
 * @param outDp Buiten/woning dauwpunt
 */
void evaluateKasVentilationDecision(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp);

/**
 * @brief Evalueert of de woning geventileerd moet worden met buitenlucht.
 */
void evaluateHomeVentilationDecision(float inTemp, float inHum, float outTemp, float outHum);

#endif // DECISION_ENGINE_H