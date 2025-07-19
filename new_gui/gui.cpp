#include "gui.h"
#include "dialog.hpp"

#include <cassert>
#include <atomic>
#include <thread>

#include <QtWidgets/QApplication>

enum class GuiState {
	Init,
	Running,
	Stopping,
	Stopped,
};

static pwasio_node_or_default get_pwio_node(PwIODeviceChooser const& picker) {
	if (picker.deviceSelected == 0)
		return PWASIO_NODE_DEFAULT;
	if (picker.deviceSelected == ~0U)
		return PWASIO_NODE_NONE;
	return picker.availableDevices[picker.deviceSelected - 1].node;
}

struct pwasio_gui {
	struct pwasio_gui_conf *conf;
	PwAsioDialog *dialog;
	std::thread thread;
	std::atomic<GuiState> state;

	pwasio_gui(struct pwasio_gui_conf *conf)
		: conf(conf)
		, dialog(nullptr)
		, thread()
		, state(GuiState::Init)
	{}

	~pwasio_gui() {
		delete dialog;
	}

	void init() {
		dialog = new PwAsioDialog(reinterpret_cast<PwHelper::Helper *>(conf->pw_helper));
		QObject::connect(
			dialog, QOverload<int>::of(&QDialog::finished),
			[this] (int status) {
				puts("WINDOW CLOSED");
				if (status == QDialog::Accepted) {
					this->apply_config();
				}
				this->conf->closed(this->conf);
			});
		load_config();
		dialog->showNormal();
	}

	void load_config() {
		conf->load_config(conf);
		dialog->setBufferSize(conf->cf_buffer_size);
	}

	void apply_config() {
		conf->cf_buffer_size = dialog->getBufferSize();
		conf->cf_io_type = static_cast<pwasio_io_config>(dialog->getIOConfigurationType());
		switch (conf->cf_io_type) {
			case PWASIO_IO_SIMPLE:
				conf->cf_io_config.simple.input  = get_pwio_node(dialog->getSimpleInputChooser());
				conf->cf_io_config.simple.output = get_pwio_node(dialog->getSimpleOutputChooser());
				break;
			case PWASIO_IO_ADVANCED:
				// TODO
				conf->cf_io_config.advanced.nodes = nullptr;
				conf->cf_io_config.advanced.cnt_nodes = 0;
				conf->cf_io_config.advanced.cnt_inputs = 0;
				conf->cf_io_config.advanced.cnt_outputs = 0;
				conf->cf_io_config.advanced.inputs = nullptr;
				conf->cf_io_config.advanced.outputs = nullptr;
				break;
		}
		conf->apply_config(conf);
	}
};

static void run_gui(struct pwasio_gui *gui) {
	gui->state.wait(GuiState::Init);
	gui->thread.detach();
	int argc = 0;
	QApplication app(argc, nullptr);
	app.setApplicationName("PipeWire ASIO Settings");
	gui->init();
	QApplication::exec();
	if (gui->state.exchange(GuiState::Stopped) == GuiState::Stopping) {
		delete gui;
	}
}

extern "C" {

struct pwasio_gui *pwasio_init_gui(struct pwasio_gui_conf *conf) {
	auto *gui = new struct pwasio_gui(conf);
	gui->thread = std::thread(run_gui, gui);
	gui->state.store(GuiState::Running);
	gui->state.notify_one();
	return gui;
}

void pwasio_destroy_gui(struct pwasio_gui *gui) {
	GuiState prev_state = GuiState::Running;
	if (!gui->state.compare_exchange_strong(prev_state, GuiState::Stopping)) {
		assert(gui->state.load() == GuiState::Stopped);
		delete gui;
	}
}

}
