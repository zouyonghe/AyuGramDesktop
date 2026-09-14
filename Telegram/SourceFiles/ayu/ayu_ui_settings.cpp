// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_ui_settings.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace AyuUiSettings {
namespace {

auto MonoFont = QString();
auto WideMultiplier = 1.;
auto MaterialSwitches = false;
auto AvatarCorners = kMaxAvatarCorners;

} // namespace

void setMonoFont(QString newFont) {
	MonoFont = std::move(newFont);
}

QString getMonoFont() {
	return MonoFont;
}

void setWideMultiplier(double val) {
	WideMultiplier = val;
}

bool isWideMultiplied() {
	return std::abs(WideMultiplier - 1.) > 0.01;
}

int getWideMultiplied(int width, double mult) {
	if (!isWideMultiplied()) {
		return width;
	}
	return std::max(width, int(std::round(width * WideMultiplier * mult)));
}

void setMaterialSwitches(bool val) {
	MaterialSwitches = val;
}

bool isMaterialSwitches() {
	return MaterialSwitches;
}

void setAvatarCorners(int val) {
	AvatarCorners = val;
}

int getAvatarCorners() {
	return AvatarCorners;
}

} // namespace AyuUiSettings
