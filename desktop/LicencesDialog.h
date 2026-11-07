#pragma once
#include <QString>

class QWidget;

// The third-party notices shown by Help > About > Licences, from the licences/ folder built in as a Qt resource.
QString thirdPartyNotices();
void showLicences(QWidget* parent);
