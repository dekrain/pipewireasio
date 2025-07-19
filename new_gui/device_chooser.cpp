#include "device_chooser.hpp"
#include "device_chooser.moc"

PwIODeviceChooser::PwIODeviceChooser(QWidget *parent)
	: QComboBox(parent)
{}

PwIODeviceChooser::~PwIODeviceChooser() = default;

void PwIODeviceChooser::showPopup() {
	emit listOpened();
	clear();
	addItem("<none>");
	addItem("<default>");
	for (auto& item : availableDevices) {
		addItem(item.name);
	}
	QComboBox::showPopup();
}

void PwIODeviceChooser::hidePopup() {
	deviceSelected = static_cast<uint32_t>(currentIndex()) - 1;
	emit listClosed();
	QComboBox::hidePopup();
}
