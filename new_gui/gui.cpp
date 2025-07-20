#include "gui.h"
#include "gui_priv.hpp"
#include "gui_priv.moc"
#include "dialog.hpp"

#include <cassert>
#include <atomic>
#include <thread>
#include <pthread.h>

#include <QtCore/QThread>
#include <QtWidgets/QApplication>

enum class GuiState {
	Init,
	Running,
	Stopping,
	Stopped,
};

enum class GlobalState {
	Init,
	SettingUp,
	Running,
};

static pwasio_node_or_default get_pwio_node(PwIODeviceChooser const& picker) {
	if (picker.deviceSelected == 0)
		return PWASIO_NODE_DEFAULT;
	if (picker.deviceSelected == ~0U)
		return PWASIO_NODE_NONE;
	return picker.availableDevices[picker.deviceSelected - 1].node;
}

struct pwasio_gui_global {
	std::atomic<GlobalState> state = GlobalState::Init;
	std::thread thread;
	QThread* qt_thread;
	int qt_custom_event;

	static void gui_thread_run();

	void wait_init() {
		GlobalState actual = GlobalState::Init;
		if (state.compare_exchange_weak(actual, GlobalState::SettingUp)) {
			// Spin up the thread
			thread = std::thread(gui_thread_run);
		}

		state.wait(GlobalState::SettingUp);
	}

	void init_gui(struct pwasio_gui* gui);
	void destroy_gui(struct pwasio_gui* gui);
};

static pwasio_gui_global global_state;

GuiEvent::GuiEvent(Type t, struct pwasio_gui* gui)
	: QEvent(static_cast<QEvent::Type>(global_state.qt_custom_event)), gui_type(t), gui(gui)
{}

void pwasio_gui_global::gui_thread_run() {
	pthread_setname_np(pthread_self(), "pwasio-gui");
	int argc = 0;
	QApplication app(argc, nullptr);
	app.setQuitOnLastWindowClosed(false);
	app.setApplicationName("PipeWire ASIO Settings");
	global_state.qt_custom_event = QEvent::registerEventType();
	app.installEventFilter(new GuiEventFilter);
	global_state.qt_thread = app.thread();
	global_state.state.store(GlobalState::Running);
	global_state.state.notify_all();
	QApplication::exec();
}

struct pwasio_gui {
	struct pwasio_gui_conf *conf;
	PwAsioDialog *dialog;
	std::atomic<GuiState> state;

	pwasio_gui(struct pwasio_gui_conf *conf)
		: conf(conf)
		, dialog(nullptr)
		, state(GuiState::Init)
	{}

	// Called from the Qt application thread
	void init() {
		dialog = new PwAsioDialog(reinterpret_cast<PwHelper::Helper *>(conf->pw_helper));
		QObject::connect(
			dialog, QOverload<int>::of(&QDialog::finished),
			[this] (int status) {
				puts("WINDOW CLOSED");
				if (state.load() == GuiState::Stopping)
					return;
				if (status == QDialog::Accepted) {
					this->apply_config();
				}
				this->conf->closed(this->conf);
			});
		load_config();
		dialog->show();
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

	void grab_focus() {
		if (dialog) {
			dialog->activateWindow();
			dialog->raise();
		}
	}
};

bool GuiEventFilter::eventFilter(QObject* obj, QEvent* ev) {
	if (ev->type() != global_state.qt_custom_event)
		return false;

	auto* gev = static_cast<GuiEvent*>(ev);
	switch (gev->gui_type) {
		case GuiEvent::Init:
			gev->gui->init();
			gev->gui->state.store(GuiState::Running);
			gev->gui->state.notify_all();
			break;
		case GuiEvent::Destroy:
			gev->gui->dialog->close();
			delete gev->gui->dialog;
			gev->gui->dialog = NULL;
			gev->gui->state.store(GuiState::Stopped);
			gev->gui->state.notify_all();
			break;
	}

	return true;
}

void pwasio_gui_global::init_gui(struct pwasio_gui* gui) {
	wait_init();
	assert(qt_thread);

	auto* ev = new GuiEvent(GuiEvent::Init, gui);
	QApplication::postEvent(qApp, ev);
	gui->state.wait(GuiState::Init);
}

void pwasio_gui_global::destroy_gui(struct pwasio_gui* gui) {
	assert(qt_thread);

	GuiState prev_state = GuiState::Running;
	if (!gui->state.compare_exchange_strong(prev_state, GuiState::Stopping)) {
		if (prev_state == GuiState::Stopping) {
			std::puts("[WARNING] Stopping GUI twice");
			gui->state.wait(GuiState::Stopping);
			return;
		}
		std::puts("GUI double free?");
		assert(prev_state == GuiState::Stopped);
		std::abort();
	}

	auto* ev = new GuiEvent(GuiEvent::Destroy, gui);
	QApplication::postEvent(qApp, ev);
	gui->state.wait(GuiState::Stopping);
	delete gui;
}

extern "C" {

struct pwasio_gui *pwasio_init_gui(struct pwasio_gui_conf *conf) {
	auto *gui = new struct pwasio_gui(conf);
	global_state.init_gui(gui);
	return gui;
}

void pwasio_destroy_gui(struct pwasio_gui *gui) {
	if (gui)
		global_state.destroy_gui(gui);
}

void pwasio_gui_focus(struct pwasio_gui *gui) {
	gui->grab_focus();
}

}
