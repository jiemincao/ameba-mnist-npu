/*
 * Ameba Edge AI (AmebaPro2 / AMB82-MINI) - Hello World
 * 目的：確認工具鏈、板子選擇、燒錄流程都正常，再進 AI 範例。
 */

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.println("AmebaPro2 blink start");
}

void loop() {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
    Serial.println("tick");
}
