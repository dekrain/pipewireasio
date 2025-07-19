#pragma once

#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QAbstractButton>

#include "ui_dialog.hpp"
#include "device_chooser.hpp"
#include "device_selector.hpp"

#include "../pw_helper.hpp"

class PwAsioDialog : public QDialog {
	Q_OBJECT

	public:
	PwAsioDialog(PwHelper::Helper *pw_helper);
	~PwAsioDialog() override;

	Q_PROPERTY(uint32_t bufferSize READ getBufferSize WRITE setBufferSize)

	inline uint32_t getBufferSize() const { return bufferSize; };
	void setBufferSize(uint32_t bufferSize);

	int getIOConfigurationType() const;

	PwIODeviceChooser const& getSimpleInputChooser() const { return *ui.input_devices; }
	PwIODeviceChooser const& getSimpleOutputChooser() const { return *ui.output_devices; }

	private slots:
	void layoutButtonClicked(QAbstractButton *button);
	void bufferSizeSet(QString const &text);
	//void dialogClosed(int status);

	inline void inputSelectorOpened() { ioSelectorOpened(PwIODeviceKind::Input); }
	inline void inputSelectorClosed() { ioSelectorClosed(PwIODeviceKind::Input); }
	inline void outputSelectorOpened() { ioSelectorOpened(PwIODeviceKind::Output); }
	inline void outputSelectorClosed() { ioSelectorClosed(PwIODeviceKind::Output); }
	void inputDeviceSelected(int slot);
	void outputDeviceSelected(int slot);

	private:
	void refreshDevices(std::vector<DeviceInfo>& nodes, PwIODeviceKind kind);
	void ioSelectorOpened(PwIODeviceKind kind);
	void ioSelectorClosed(PwIODeviceKind kind);

	Ui::PwAsioDialog ui;
	QButtonGroup *layoutGroup;

	uint32_t bufferSize;

	PwHelper::Helper *pw_helper;
};
