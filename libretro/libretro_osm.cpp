#include "UI/OnScreenDisplay.h"

OnScreenMessages osm;

void OnScreenMessages::Draw(DrawBuffer &draw, const Bounds &bounds) {
	//(void)draw;
   //(void)bounds;
}

void OnScreenMessages::Show(const std::string &message, float duration_s, uint32_t color, int icon, bool checkUnique) {
	(void)message;
   (void)duration_s;
   (void)color;
   (void)icon;
   (void)checkUnique;
}

void OnScreenMessages::ShowOnOff(const std::string &message, bool b, float duration_s, uint32_t color, int icon) {
	(void)message;
   (void)b;
   (void)duration_s;
   (void)color;
   (void)icon;
}
