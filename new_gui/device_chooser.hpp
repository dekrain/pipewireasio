#pragma once

#include <QtWidgets/QComboBox>

#include <vector>

struct DeviceInfo {
	struct pw_node *node;
	QString name;
};

class PwIODeviceChooser: public QComboBox {
	Q_OBJECT

	public:
	PwIODeviceChooser(QWidget *parent);
	~PwIODeviceChooser() override;

	// 0 for default and 1.. from the list
	// ~0 = deactivated
	uint32_t deviceSelected;

	std::vector<DeviceInfo> availableDevices;

	signals:
	// Used to inform the user to lock devices and fill in the available device list.
	void listOpened();
	// Used to inform the user to unlock devices and store the selected device.
	void listClosed();

	protected:
	void showPopup() override;
	void hidePopup() override;
};
