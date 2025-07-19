#include "dialog.hpp"
#include "dialog.moc"
#include "../pw_helper.hpp"

#include <QtCore/qglobal.h>
#include <QtGui/QIntValidator>
#include <QtWidgets/qabstractbutton.h>
#include <QtWidgets/qbuttongroup.h>

#include "device_chooser.hpp"
#include "device_selector.hpp"

#include <pipewire/keys.h>

PwAsioDialog::PwAsioDialog(PwHelper::Helper *helper)
	: layoutGroup(new QButtonGroup)
	, pw_helper(helper)
{
	ui.setupUi(this);
	layoutGroup->addButton(ui.layoutButton_0, 0);
	layoutGroup->addButton(ui.layoutButton_1, 1);
	ui.bufferSize->setValidator(new QIntValidator(1, INT_MAX, this));
	connect(
		layoutGroup, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
		this, QOverload<QAbstractButton *>::of(&PwAsioDialog::layoutButtonClicked));
}

PwAsioDialog::~PwAsioDialog() {
	delete layoutGroup;
}

void PwAsioDialog::setBufferSize(uint32_t newBufferSize) {
	bufferSize = newBufferSize;
	ui.bufferSize->setCurrentText(QString::asprintf("%u", newBufferSize));
}

int PwAsioDialog::getIOConfigurationType() const {
	return ui.io_config->currentIndex();
}

void PwAsioDialog::layoutButtonClicked([[maybe_unused]] QAbstractButton *button) {
	//int bid = layoutGroup->id(button);
	//printf("[DBG] Button clicked: %d\n", bid);
	auto *checked = layoutGroup->checkedButton();
	int cid = layoutGroup->id(checked);
	//printf("[DBG] Currently checked: %d\n", cid);
	ui.io_config->setCurrentIndex(cid);
}

void PwAsioDialog::bufferSizeSet(QString const &text) {
	bool ok = false;
	uint32_t size = text.toUInt(&ok);
	if (!ok)
		return;

	printf("Buffer size changed: %u\n", size);
	bufferSize = size;
}

void PwAsioDialog::refreshDevices(std::vector<DeviceInfo>& out_nodes, PwIODeviceKind kind) {
	auto nodes = PwHelper::enumerate_pipewire_endpoints(pw_helper);
	out_nodes.clear();

	for (auto* node: nodes) {
		std::string s_name, s_descr;
		std::string media_class;
		std::pair<std::string_view, std::string*> props[] {
			{PW_KEY_NODE_NAME, &s_name},
			{PW_KEY_NODE_DESCRIPTION, &s_descr},
			{PW_KEY_MEDIA_CLASS, &media_class},
		};
		PwHelper::get_node_props(pw_helper, node, props);

		using namespace std::string_view_literals;

		if (s_descr.empty())
			s_descr = std::move(s_name);

		bool matches = false;
		if (media_class == "Audio/Source"sv) {
			matches = kind == PwIODeviceKind::Input;
			puts("[DEBUG] Discovered an input");
		} else if (media_class == "Audio/Sink"sv) {
			matches = kind == PwIODeviceKind::Output;
			puts("[DEBUG] Discovered an output");
		} else {
			printf("[DEBUG] Discovered an unknown kind of node: %s\n", media_class.c_str());
		}

		if (matches) {
			out_nodes.emplace_back(node, QString::fromStdString(s_descr));
		}
	}
}

void PwAsioDialog::ioSelectorOpened(PwIODeviceKind kind) {
	refreshDevices(kind == PwIODeviceKind::Input ?
		ui.input_devices->availableDevices :
		ui.output_devices->availableDevices, kind);
}

void PwAsioDialog::ioSelectorClosed(PwIODeviceKind kind) {}

void PwAsioDialog::inputDeviceSelected(int slot) {}

void PwAsioDialog::outputDeviceSelected(int slot) {}
