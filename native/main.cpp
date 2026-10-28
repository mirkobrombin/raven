#include "pe/pe.h"
#include "pe/resources.h"
#include "pe/loaded_image.h"
#include "pe/module_set.h"
#ifdef RAVEN_NATIVE_CPU
#include "cpu/native_cpu.h"
#include "nt/providers.h"
#include "nt/paths.h"
#include "nt/launch.h"
#include "nt/actctx.h"
#include "nt/elevation.h"
#include "nt/restart.h"
#include "nt/snapshot.h"
#include "nt/handle_channel.h"
#include "nt/text.h"
#include "nt/system_exports.h"
#ifdef RAVEN_OPENGL
#include "graphics/opengl_bridge.h"
#endif
#ifdef RAVEN_WAYLAND
#include "desktop/wayland.h"
#endif
#ifdef RAVEN_XCB
#include "desktop/xcb.h"
#endif
#endif

#include <set>
#include <filesystem>
#include <charconv>
#include <chrono>
#include <string_view>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdexcept>

static std::vector<uint8_t> read_file(const char *path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file)
		throw std::runtime_error("cannot open PE file");
	const auto size = file.tellg();
	if (size < 0 || size > 1024 * 1024 * 1024)
		throw std::runtime_error("PE file exceeds inspection size limit");
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), size))
		throw std::runtime_error("cannot read PE file");
	return bytes;
}
#ifdef RAVEN_NATIVE_CPU
static raven::ModuleBytes read_module(const std::filesystem::path &path)
{
	auto bytes = raven::ModuleBytes::map(path);
	if (!bytes)
		throw std::runtime_error("cannot map PE file");
	if (std::span<const uint8_t>(*bytes).size() > 1024 * 1024 * 1024)
		throw std::runtime_error("PE file exceeds inspection size limit");
	return std::move(*bytes);
}

// A program started by another one leaves its whole exit code here for the parent; the host exit status keeps only its low byte.
static int exit_code_fd=-1;

static void leave_exit_code(uint32_t code)
{
	if (exit_code_fd>=0)
		[[maybe_unused]] const auto written=::pwrite(exit_code_fd,&code,sizeof code,0);
}

static int native_result(const raven::NativeResult &result,bool process_exit=false,bool inherited_stdio=false)
{
	if (result.stop==raven::NativeStop::exited) {
		const auto code=uint32_t(result.value);
		leave_exit_code(code);
		if (!inherited_stdio) std::cout << "Native process exited with status " << code << '\n';
		return !code ? 0 : (code & 0xff) ? int(code & 0xff) : 1;
	}
	if (result.stop == raven::NativeStop::returned) {
		if (!inherited_stdio) std::cout << "Native entry returned " << result.value << " (process initialization not implemented)\n";
		const auto code=uint32_t(result.value);
		if (process_exit)
			leave_exit_code(code);
		return !process_exit || !code ? 0 : (code & 0xff) ? int(code & 0xff) : 1;
	}
	if (result.stop == raven::NativeStop::syscall)
		std::cerr << "raven: unsupported guest syscall " << result.value;
	else if (result.stop == raven::NativeStop::timeout)
		std::cerr << "raven: native entry timed out";
	else if (result.stop == raven::NativeStop::api)
		std::cerr << "raven: unimplemented native process API";
	else if (result.stop == raven::NativeStop::exception)
		std::cerr << "raven: structured exception 0x" << std::hex << result.value << " (guest handler dispatch not implemented)";
	else
		std::cerr << "raven: native guest fault signal " << result.signal;
	std::cerr << " at 0x" << std::hex << result.instruction;
	if (result.stop == raven::NativeStop::fault && result.fault_address)
		std::cerr << " (address 0x" << result.fault_address << ")";
	std::cerr << '\n';
	return 1;
}
#endif

#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <exception>
#include <execinfo.h>

// With RAVEN_BACKTRACE set, an exception nothing catches shows where it came from.
static void show_backtrace()
{
	void *frames[48];
	const int count = backtrace(frames, 48);
	backtrace_symbols_fd(frames, count, 2);
	std::abort();
}

// With RAVEN_BACKTRACE set, a fault of the runtime itself shows where it happened before the process ends with the signal.
static void show_fault(int signal)
{
	void *frames[48];
	const int count = backtrace(frames, 48);
	backtrace_symbols_fd(frames, count, 2);
	std::signal(signal, SIG_DFL);
	std::raise(signal);
}

int main(int argc, char **argv)
{
	if (std::getenv("RAVEN_BACKTRACE")) {
		std::set_terminate(show_backtrace);
		std::signal(SIGSEGV, show_fault);
		std::signal(SIGBUS, show_fault);
	}
	// RAVEN_TRACE_FILE=path sends the diagnostics of every process, a child program's included, to its own file path.<pid>.
	if (const char *trace = std::getenv("RAVEN_TRACE_FILE")) {
		const auto name = std::string(trace) + "." + std::to_string(::getpid());
		if (!std::freopen(name.c_str(), "w", stderr))
			return 2;
		// A file is fully buffered once opened; diagnostics are read while the process runs, a line at a time.
		std::setvbuf(stderr, nullptr, _IOLBF, 0);
	}
	const std::string mode = argc > 1 ? argv[1] : "";
	// A restart of Windows: the moves and deletions installers left for it, then the programs that run once at the next logon.
	if (mode == "--reboot") {
		const auto done = raven::simulate_restart(raven::NativePrefix::from_environment(), std::filesystem::read_symlink("/proc/self/exe"));
		std::cout << "restarted: " << done << " pending operations and programs ran\n";
		return 0;
	}
	// Snapshots of the prefix: taken by hand, listed, and restored, the newest unless one is named.
	if (mode == "--snapshot-prefix" || mode == "--restore-prefix" || mode == "--list-snapshots") {
		const auto prefix = raven::NativePrefix::from_environment().root();
		try {
			if (mode == "--snapshot-prefix") {
				std::cout << raven::take_snapshot(prefix, argc > 2 ? argv[2] : "manual").filename().string() << '\n';
			} else if (mode == "--restore-prefix") {
				const auto kept = raven::restore_snapshot(prefix, argc > 2 ? std::optional<std::string>(argv[2]) : std::nullopt);
				std::cout << "restored; what the prefix held before is the snapshot " << kept.filename().string() << '\n';
			} else {
				for (const auto &snapshot : raven::list_snapshots(prefix))
					std::cout << snapshot.filename().string() << '\n';
			}
		} catch (const std::exception &failure) {
			std::cerr << "raven: " << failure.what() << '\n';
			return 1;
		}
		return 0;
	}
	const bool dump_image = mode == "--dump-image";
	const bool map_image = mode == "--map-image" || dump_image;
	const bool run_entry = mode == "--run-entry";
	const bool run_modules = mode == "--run-modules";
	const bool run_process = mode == "--run-process";
	int first_file=3;
	auto timeout=std::chrono::milliseconds(5000);
	bool timeout_given=false;
	bool wayland=false,x11=false;
	if (run_process && argc>3 && std::string_view(argv[3]).starts_with("--timeout-ms=")) {
		const std::string_view value=std::string_view(argv[3]).substr(13);
		uint64_t milliseconds=0;
		const auto parsed=std::from_chars(value.data(),value.data()+value.size(),milliseconds);
		if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || milliseconds<1 || milliseconds>86400000) {
			std::cerr << "raven: native timeout must be 1..86400000 milliseconds\n";
			return 2;
		}
		timeout=std::chrono::milliseconds(milliseconds);
		timeout_given=true;
		first_file=4;
	}
	if (run_process && argc>first_file && std::string_view(argv[first_file])=="--desktop=wayland") {
		wayland=true; ++first_file;
	} else if (run_process && argc>first_file && std::string_view(argv[first_file])=="--desktop=x11") {
		x11=true; ++first_file;
	}
	bool inherited_stdio=false;
	if (run_process && argc>first_file && std::string_view(argv[first_file])=="--stdio=inherit") {
		inherited_stdio=true; ++first_file;
	}
	// A program started by another one carries the command line and the directory its parent chose.
	std::optional<std::string> command_line_text, directory_text;
	int environment_fd=-1,suspended_fd=-1,inherit_fd=-1,handle_channel_fd=-1,guest_error_fd=2;
	bool elevated=false,noninteractive=false;
	for (bool more=run_process;more && argc>first_file;) {
		const std::string_view option=argv[first_file];
		if (option.starts_with("--command-line=")) command_line_text=std::string(option.substr(15));
		else if (option.starts_with("--directory=")) directory_text=std::string(option.substr(12));
		else if (option.starts_with("--environment-fd=")) environment_fd=std::atoi(std::string(option.substr(17)).c_str());
		else if (option.starts_with("--suspended-fd=")) {
			suspended_fd=std::atoi(std::string(option.substr(15)).c_str());
			::fcntl(suspended_fd,F_SETFD,FD_CLOEXEC);
		}
		else if (option.starts_with("--inherit-fd=")) inherit_fd=std::atoi(std::string(option.substr(13)).c_str());
		else if (option.starts_with("--handle-channel-fd=")) handle_channel_fd=std::atoi(std::string(option.substr(20)).c_str());
		else if (option.starts_with("--diagnostics-fd=")) {
			// The runtime's own messages go where the parent runtime writes its own; the program's standard error moves out of their way
			// to a descriptor of its own, so nothing the runtime says ever lands in a pipe the program reads.
			const int diagnostics=std::atoi(std::string(option.substr(17)).c_str());
			const int moved=::fcntl(2,F_DUPFD_CLOEXEC,64);
			if (moved>=0 && ::dup2(diagnostics,2)==2) guest_error_fd=moved;
			else if (moved>=0) ::close(moved);
			::close(diagnostics);
		}
#ifdef RAVEN_NATIVE_CPU
		else if (option.starts_with("--exit-code-fd=")) {
			exit_code_fd=std::atoi(std::string(option.substr(15)).c_str());
			::fcntl(exit_code_fd,F_SETFD,FD_CLOEXEC);
		}
#endif
		else if (option=="--elevated") elevated=true;
		else if (option=="--noninteractive") noninteractive=true;
		else more=false;
		if (more) ++first_file;
	}
	// What programs it starts inherit: how long they may run and which window system they use.
	const auto pass_options=[&] {
		std::string options="--timeout-ms="+std::to_string(timeout.count());
		if (wayland) options+=" --desktop=wayland";
		if (x11) options+=" --desktop=x11";
		setenv("RAVEN_CHILD_OPTIONS",options.c_str(),1);
	};
	if (run_process) pass_options();
	const bool bind_imports = mode == "--bind-imports";
	int last_file=argc;
	if (run_process)
		for (int i=first_file;i<argc;++i)
			if (std::string_view(argv[i])=="--") { last_file=i;break; }
	const bool inspect = mode == "--inspect" || mode == "--inspect-imports" || mode == "--inspect-exports" ||
		mode == "--inspect-delay-imports" || mode == "--inspect-resources";
	const bool valid = (inspect && argc == 3) || (map_image && (argc == 3 || argc == 4)) ||
		(run_entry && argc == 4 && std::string(argv[2]) == "--execution=native") || (bind_imports && argc >= 3) ||
		((run_modules || run_process) && last_file > first_file && std::string(argv[2]) == "--execution=native");
	if (!valid) {
		std::cerr << "Usage: raven --inspect|--inspect-imports|--inspect-exports|--inspect-delay-imports|--inspect-resources program.exe\n"
			<< "       raven --map-image|--dump-image program.exe [base-address]\n"
			<< "       raven --bind-imports program.exe dependency.dll ...\n"
			<< "       raven --run-entry --execution=native fixture.exe\n"
			<< "       raven --run-modules --execution=native fixture.exe dependency.dll ...\n"
			<< "       raven --run-process --execution=native [--timeout-ms=N] [--desktop=wayland|--desktop=x11] [--stdio=inherit] program.exe [dependency.dll ...] [-- guest-argument ...]\n"
			<< "Complete Windows process initialization is not implemented yet.\n";
		return 2;
	}
	try {
		if (bind_imports || run_modules || run_process) {
			raven::ModuleSet modules;
			raven::PEModule *entry = nullptr;
			for (int i = (run_modules || run_process) ? first_file : 2; i < last_file; ++i) {
				auto bytes = read_file(argv[i]);
				if (!entry && run_process)
					raven::widen_il_only_image(bytes);
				auto &module = modules.add(std::filesystem::path(argv[i]).filename().string(), std::move(bytes));
				if (!entry) {
					entry = &module;
					// A program runs elevated when its manifest asks for it or when the process that started it is elevated.
					raven::set_process_elevated(elevated || raven::manifest_requests_elevation(module.image, std::filesystem::path(argv[i]).filename().string()));
					// The Windows Installer server, which msiexec runs as with -Embedding, works in the services' session, and so do the
					// custom actions it starts.
					auto program = std::filesystem::path(argv[i]).filename().string();
					for (auto &character : program)
						if (character >= 'A' && character <= 'Z')
							character = char(character + ('a' - 'A'));
					raven::set_process_noninteractive(noninteractive || (program == "msiexec.exe" && command_line_text &&
						command_line_text->find("-Embedding") != std::string::npos));
					// RAVEN_PARALLEL_PROGRAM runs only the named program's threads in parallel; each program it starts decides again.
					if (const char *parallel = std::getenv("RAVEN_PARALLEL_PROGRAM")) {
						std::string wanted = parallel;
						for (auto &character : wanted)
							if (character >= 'A' && character <= 'Z')
								character = char(character + ('a' - 'A'));
						if (program == wanted)
							::setenv("RAVEN_PARALLEL", "1", 1);
						else
							::unsetenv("RAVEN_PARALLEL");
					}
				}
			}
			if (run_modules || run_process) {
#ifdef RAVEN_NATIVE_CPU
				const auto bootstrap = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "raven-bootstrap";
				if (run_process) {
					const auto directory = std::filesystem::absolute(argv[first_file]).parent_path();
					const raven::NativePrefix prefix=raven::NativePrefix::from_environment();
					// The system's own programs, libraries and tables the runtime ships, in the system directory of each width as Windows has them; a
					// copy that is missing or differs from the runtime's is put back.
					{
						const auto runtime = std::filesystem::read_symlink("/proc/self/exe").parent_path();
						for (const auto &[from, folder] : {std::pair{runtime, "system32"}, std::pair{runtime / "x86", "syswow64"}})
							for (const char *program : {"cmd.exe", "msiexec.exe", "regsvr32.exe", "control.exe", "dism.exe", "fondue.exe", "regedit.exe", "mscoree.dll", "riched20.dll",
									"msftedit.dll", "l_intl.nls", "find.exe", "findstr.exe", "more.com", "sort.exe", "where.exe", "xcopy.exe", "attrib.exe",
									"timeout.exe", "choice.exe", "whoami.exe", "hostname.exe", "tasklist.exe", "taskkill.exe", "reg.exe"}) {
								std::error_code error;
								// The 64-bit Registry Editor lives in the Windows folder itself.
								const bool windows_folder = std::string_view(program) == "regedit.exe" && std::string_view(folder) == "system32";
								const auto source_file = from / program,
									target = windows_folder ? prefix.root() / "drive_c" / "windows" / program : prefix.root() / "drive_c" / "windows" / folder / program;
								if (!std::filesystem::exists(source_file, error))
									continue;
								if (std::filesystem::exists(target, error) && std::filesystem::file_size(target, error) == std::filesystem::file_size(source_file, error) &&
									std::filesystem::last_write_time(target, error) >= std::filesystem::last_write_time(source_file, error))
									continue;
								std::filesystem::copy_file(source_file, target, std::filesystem::copy_options::overwrite_existing, error);
							}
						// The classes the runtime's components serve are registered in the prefix, as Windows ships them registered: regsvr32 runs
						// for each width when the components changed since the prefix last registered them. The runs themselves skip this step.
						const char *servers[] = {"msxml3.dll", "windowscodecs.dll", "mlang.dll", "propsys.dll"};
						std::string stamp;
						for (const auto &from : {runtime, runtime / "x86"})
							for (const char *server : servers) {
								std::error_code error;
								const auto file = from / server;
								if (std::filesystem::exists(file, error))
									stamp += file.string() + " " + std::to_string(std::filesystem::file_size(file, error)) + " " +
										std::to_string(std::filesystem::last_write_time(file, error).time_since_epoch().count()) + "\n";
							}
						const auto marker = prefix.root() / "drive_c" / "windows" / "raven-registered.txt";
						std::string recorded;
						if (std::ifstream input(marker); input)
							recorded.assign(std::istreambuf_iterator<char>(input), {});
						// Children use the prefix preparation already attempted by the process that started them.
						if (handle_channel_fd < 0 && !std::getenv("RAVEN_PREFIX_SETUP") && !stamp.empty() && stamp != recorded) {
							// The prefix counts as registered only when every registration succeeded; a failed one is tried again next time.
							bool registered = true;
							for (const char *folder : {"system32", "syswow64"}) {
								const auto program = prefix.root() / "drive_c" / "windows" / folder / "regsvr32.exe";
								if (!std::filesystem::exists(program))
									continue;
								for (const char *server : servers) {
									const pid_t child = ::fork();
									if (child == 0) {
										::setenv("RAVEN_PREFIX_SETUP", "1", 1);
										// Setup is quiet: only a failure says anything, on standard error.
										if (const int quiet = ::open("/dev/null", O_WRONLY | O_CLOEXEC); quiet >= 0)
											::dup2(quiet, 1);
										const std::string self = std::filesystem::read_symlink("/proc/self/exe").string(), path = program.string();
										::execl(self.c_str(), self.c_str(), "--run-process", "--execution=native", "--timeout-ms=60000", path.c_str(), "--", "/s",
											server, static_cast<char *>(nullptr));
										::_exit(127);
									}
									int status = 0;
									if (child > 0)
										while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
									registered = registered && child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
								}
							}
							if (registered)
								std::ofstream(marker) << stamp;
						}
					}
					// Before an installation the prefix is kept as a snapshot, which `raven --restore-prefix` brings back: for a program that looks
					// like an installer and that no other program started. The ten newest of these snapshots are kept.
					const bool started_alone = !command_line_text && handle_channel_fd < 0;
					const auto program_name = std::filesystem::path(argv[first_file]).filename().string();
					if (started_alone && !std::getenv("RAVEN_PREFIX_SETUP") && !std::getenv("RAVEN_NO_SNAPSHOT") && raven::installer_like(entry->image, program_name)) {
						try {
							raven::take_snapshot(prefix.root(), "install-" + std::filesystem::path(program_name).stem().string());
							raven::prune_snapshots(prefix.root(), "-install-", 10);
						} catch (const std::exception &failure) {
							std::cerr << "raven: no snapshot of the prefix before the installation: " << failure.what() << '\n';
						}
					}
					// The libraries installed for every program, in the system directory of the program's width.
					const auto system_directory = prefix.root() / "drive_c" / "windows" / (entry->image.is_64 ? "system32" : "syswow64");
					// The program's own manifest names the side-by-side assemblies its libraries come from; it is read from the file, as the
					// first libraries are found before the process exists.
					{
						std::string manifest;
						for (const auto &resource : entry->image.resources())
							if (resource.type == raven::ResourceKey{uint32_t(24)} && resource.name == raven::ResourceKey{uint32_t(1)} && resource.size < 1024 * 1024) {
								const auto bytes = entry->image.file_span(resource.data_address, resource.size);
								manifest.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
								break;
							}
						raven::activation_contexts().prepare(prefix.root() / "drive_c" / "windows" / "WinSxS" / "Manifests", entry->image.is_64, manifest);
					}
					// A library of the other width is passed over, as the Windows loader goes on searching past it.
					const auto find_file = [&](const std::filesystem::path &folder, std::string_view name) {
						std::optional<std::filesystem::path> selected;
						std::error_code error;
						for (std::filesystem::directory_iterator walk(folder, error), end; walk != end; walk.increment(error)) {
							auto filename = walk->path().filename().string();
							for (auto &character : filename)
								if (character >= 'A' && character <= 'Z')
									character = static_cast<char>(character + ('a' - 'A'));
							if (filename != name || !walk->is_regular_file())
								continue;
							if (selected)
								throw std::runtime_error("ambiguous case-insensitive DLL filename");
							selected = walk->path();
						}
						if (selected) {
							try {
								if (raven::PEImage(read_module(*selected)).is_64 != entry->image.is_64)
									selected.reset();
							} catch (const std::exception &) {
								selected.reset();
							}
						}
						return selected;
					};
					// The components for 32-bit programs are the same libraries built for 32 bits, in a directory beside the runtime.
					const auto component_directory = std::filesystem::read_symlink("/proc/self/exe").parent_path() / (entry->image.is_64 ? "" : "x86");
					const auto source = [&](std::string_view name) -> std::optional<raven::ModuleBytes> {
#ifdef RAVEN_OPENGL
						if (entry->image.is_64 && raven::OpenGLBridge::enabled() && name == "opengl32.dll") return std::nullopt;
#endif
						// The Windows Installer, the libraries it installs with and remote procedure calls are the runtime's components, whatever copies a program
						// ships or an installer leaves in the system directory.
						if (std::find(std::begin(raven::runtime_components), std::end(raven::runtime_components), name) != std::end(raven::runtime_components))
							if (const auto component = component_directory / name; std::filesystem::exists(component))
								return read_module(component.c_str());
						// A library an assembly of the context in effect declares comes from that assembly's folder before any other place, as
						// Windows redirects it.
						if (const auto folder = raven::activation_contexts().library_folder(name))
							if (const auto redirected = find_file(*folder, name))
								return read_module(redirected->c_str());
						std::optional<std::filesystem::path> selected = find_file(directory, name);
						if (!selected && (name == "opengl32.dll" || name == "glu32.dll" || name == "libgallium_wgl.dll" || name == "libglapi.dll"))
							if (const char *graphics = std::getenv("RAVEN_OPENGL_DIRECTORY"))
								selected = find_file(graphics, name);
						// The runtime's own components are found beside the runtime when the program does not ship them.
						// A 64-bit program built against msvcrt.dll gets the C runtime component as that library too: a runtime of its own, as
						// msvcrt is beside ucrtbase on Windows.
						if (!selected && name=="msvcrt.dll" && entry->image.is_64) {
							const auto component = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "ucrtbase.dll";
							if (std::filesystem::exists(component))
								return read_module(component.c_str());
						}
						if (!selected && name=="ucrtbase.dll") {
							// A 32-bit program gets the 32-bit build of the component.
							const auto component = std::filesystem::read_symlink("/proc/self/exe").parent_path() / (entry->image.is_64 ? "ucrtbase.dll" : "ucrtbase32.dll");
							if (std::filesystem::exists(component))
								return read_module(component.c_str());
						}
						// DirectWrite, Direct2D and Windows Animation are the runtime's own components too: the library names resolve to them.
						if (!selected && (name=="dwrite.dll" || name=="d2d1.dll" || name=="uianimation.dll") && entry->image.is_64) {
							const auto component = std::filesystem::read_symlink("/proc/self/exe").parent_path() /
								(name=="dwrite.dll" ? "raven-dwrite.dll" : name=="d2d1.dll" ? "raven-d2d1.dll" : "raven-uianimation.dll");
							if (std::filesystem::exists(component))
								return read_module(component.c_str());
						}
						// The Direct3D libraries a Windows installation provides come from RAVEN_D3D_DIRECTORY (a DXVK build of the program's width)
						// or from beside the runtime.
						if (!selected && (name=="dxgi.dll" || name=="d3d11.dll" || name=="d3d10core.dll" || name=="d3d10_1.dll" || name=="d3d9.dll" || name=="d3d8.dll")) {
							std::vector<std::filesystem::path> places;
							if (const char *directory = std::getenv("RAVEN_D3D_DIRECTORY")) places.emplace_back(directory);
							places.emplace_back(std::filesystem::read_symlink("/proc/self/exe").parent_path());
							for (const auto &place : places)
								if (std::filesystem::exists(place / name))
									return read_module((place / name).c_str());
							// Direct3D 10.1 is the runtime's own library over the d3d10core.dll found here, when no build carries one.
							if (name=="d3d10_1.dll")
								if (const auto component = component_directory / "raven-d3d10_1.dll"; std::filesystem::exists(component))
									return read_module(component.c_str());
						}
						// Then the system directory, where installers put the libraries programs share. The system's own libraries are the
						// runtime's whatever files there carry their names.
						const auto system_names = raven::system_library_names();
						if (!selected && std::find(system_names.begin(), system_names.end(), name) == system_names.end())
							selected = find_file(system_directory, name);
						if (!selected)
							return std::nullopt;
						return read_module(selected->c_str());
					};
					const auto debug=[](std::span<const uint8_t> message) {
						std::cerr << "guest debug: ";
						for (const auto byte : message) {
							if ((byte>=32 && byte<127) || byte=='\n' || byte=='\t') std::cerr << char(byte);
							else std::cerr << "\\x" << std::hex << unsigned(byte) << std::dec;
						}
						std::cerr << '\n';
					};
					// The guest heap component ships beside the runtime when the build provisioned it.
					if (entry->image.is_64 && !modules.all().contains("raven-rtl.dll")) {
						const auto component = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "raven-rtl.dll";
						if (std::filesystem::exists(component))
							modules.add("raven-rtl.dll", read_file(component.c_str()));
					}
					// The libraries the program and the DLLs shipped beside it import, at once or lazily, by lowercase file name.
					const auto imported_libraries = [&] {
						std::set<std::string> names;
						std::vector<std::string> pending;
						const auto lower = [](std::string name) {
							for (auto &character : name)
								if (character >= 'A' && character <= 'Z')
									character = static_cast<char>(character + ('a' - 'A'));
							if (name.find('.') == name.npos)
								name += ".dll";
							return name;
						};
						const auto visit = [&](const std::vector<raven::PEImport> &imports) {
							for (const auto &symbol : imports)
								if (names.insert(lower(symbol.module)).second)
									pending.push_back(lower(symbol.module));
						};
						for (const auto &item : modules.all()) {
							visit(item.second->imports);
							for (const auto &descriptor : item.second->image.delay_imports())
								visit(descriptor.imports);
						}
						while (!pending.empty()) {
							const auto name = pending.back();
							pending.pop_back();
							if (const auto bytes = source(name))
							{
								const raven::PEImage image(*bytes);
								visit(image.imports());
								// A library the program loads lazily still needs its components in place.
								for (const auto &descriptor : image.delay_imports())
									visit(descriptor.imports);
							}
						}
						return names;
					}();
					// A .NET program reaches every system library through its runtime, which loads them as it needs them: it gets every component.
					const bool managed = entry->image.directories[14].address && entry->image.directories[14].size;
					const auto imports_any = [&](std::initializer_list<const char *> libraries) {
						return managed || std::any_of(libraries.begin(), libraries.end(), [&](const char *library) {
							const std::string_view wanted(library);
							// A trailing '*' names a family of API set libraries.
							if (wanted.ends_with('*'))
								return std::any_of(imported_libraries.begin(), imported_libraries.end(), [&](const auto &name) { return name.starts_with(wanted.substr(0, wanted.size() - 1)); });
							return imported_libraries.contains(std::string(wanted));
						});
					};
					if (imports_any({"opengl32.dll", "glu32.dll"})
#ifdef RAVEN_OPENGL
						&& !(entry->image.is_64 && raven::OpenGLBridge::enabled())
#endif
						)
						if (const char *graphics = std::getenv("RAVEN_OPENGL_DIRECTORY"))
							for (const char *library : {"opengl32.dll", "glu32.dll"})
								if (!modules.all().contains(library)) {
									auto file = find_file(directory, library);
									if (!file)
										file = find_file(graphics, library);
									if (file)
										modules.add(library, read_file(file->c_str()));
								}
					// The common controls component stands in for comctl32.dll when the program does not ship its own.
					if (!modules.all().contains("comctl32.dll")) {
						const auto component = component_directory / "comctl32.dll";
						if (imports_any({"comctl32.dll", "user32.dll"}) && std::filesystem::exists(component))
							modules.add("comctl32.dll", read_file(component.c_str()));
					}
					// Components that stand in for parts of system libraries are loaded beside the program that imports those libraries.
					const auto load_component = [&](const char *file, std::initializer_list<const char *> libraries) {
						if (modules.all().contains(file))
							return;
						const auto component = component_directory / file;
						if (imports_any(libraries) && std::filesystem::exists(component))
							modules.add(file, read_file(component.c_str()));
					};
					// The user32 component supplies the built-in control classes and dialog manager to programs that use user32.
					load_component("raven-user32.dll", {"user32.dll"});
					// The shell component serves every graphical program, as libraries it loads later, such as a setup program's engine, import the
					// shell, the setup API and the dialogs that the program itself may not. A program that imports automation creates classes
					// through COM too, even when it loads ole32 only later.
					load_component("raven-shell.dll", {"comdlg32.dll", "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "user32.dll"});
					// The setup API component: setup information files, file queues and cabinets, for the programs that import it and for
					// installers, whose engines and the advpack they load import it later.
					if (const auto setup = component_directory / "raven-setupapi.dll"; !modules.all().contains("raven-setupapi.dll") &&
						std::filesystem::exists(setup) && (imports_any({"setupapi.dll", "advpack.dll"}) ||
						raven::installer_like(entry->image, std::filesystem::path(argv[first_file]).filename().string())))
						modules.add("raven-setupapi.dll", read_file(setup.c_str()));
					// Structured storage, the compound files installer packages are made of, and the memory streams of COM, which programs that
					// import automation use too.
					load_component("raven-storage.dll", {"ole32.dll", "oleaut32.dll"});
					// Automation (variants, safe arrays, BSTRs, type libraries) is the component's for every program: libraries loaded later, such
					// as the XML component, use it as much as the program does.
					if (const auto automation = component_directory / "oleaut32.dll"; !modules.all().contains("oleaut32.dll") && std::filesystem::exists(automation))
						modules.add("oleaut32.dll", read_file(automation.c_str()));
					if (entry->image.is_64)
						load_component("ucrtbase.dll", {"ucrtbase.dll", "api-ms-win-crt-*"});
					raven::DesktopPresentation presentation;
#ifdef RAVEN_WAYLAND
					raven::WaylandPresentation backend;
					if (wayland) presentation=backend.callbacks();
#else
					if (wayland) throw std::runtime_error("native Wayland presentation was not built");
#endif
#ifdef RAVEN_XCB
					raven::XcbPresentation x11_backend;
					if (x11) presentation=x11_backend.callbacks();
#else
					if (x11) throw std::runtime_error("native X11 presentation was not built");
#endif
					std::optional<raven::NativeLaunch> launch;
					{
						std::vector<std::string_view> arguments;
						for (int i=last_file+1;i<argc;++i) arguments.emplace_back(argv[i]);
						// The guest sees its own executable by a drive-qualified path, like any Windows process.
						const raven::NativePaths paths(prefix);
						const auto image=paths.dos(std::filesystem::absolute(argv[first_file]));
						launch=raven::NativeLaunch{image,raven::launch_command_line(image,arguments)};
						launch->suspended_fd = suspended_fd;
						if (command_line_text) launch->command_line=raven::from_utf8(*command_line_text);
						if (directory_text) launch->directory=raven::from_utf8(*directory_text);
						if (handle_channel_fd>=0) raven::set_parent_channel(handle_channel_fd);
						if (inherit_fd>=0) {
							// The handles the creator passed on arrive as descriptors, described in an anonymous file.
							std::vector<uint8_t> bytes;
							uint8_t chunk[4096];
							for (ssize_t count;(count=::read(inherit_fd,chunk,sizeof(chunk)))!=0;) {
								if (count<0) { if (errno==EINTR) continue; break; }
								bytes.insert(bytes.end(),chunk,chunk+count);
							}
							::close(inherit_fd);
							launch->inherited=raven::parse_inherited(bytes);
						}
						if (environment_fd>=0) {
							// The parent's environment block arrives in an anonymous file as UTF-16.
							std::string bytes;
							char chunk[4096];
							for (ssize_t count;(count=::read(environment_fd,chunk,sizeof(chunk)))!=0;) {
								if (count<0) { if (errno==EINTR) continue; break; }
								bytes.append(chunk,size_t(count));
							}
							::close(environment_fd);
							std::u16string block(bytes.size()/2,u'\0');
							std::memcpy(block.data(),bytes.data(),block.size()*2);
							launch->environment=std::move(block);
						}
						// A console program started from a terminal has the terminal for its console, as Windows gives a console program the
						// console it was started from, and runs for as long as it is used.
						if (!inherited_stdio && handle_channel_fd<0 && entry->image.subsystem==3 && (::isatty(0) || ::isatty(1))) {
							inherited_stdio=true;
							if (!timeout_given) {
								timeout=std::chrono::milliseconds(86400000);
								pass_options();
							}
						}
						if (inherited_stdio) launch->standard_fds={0,1,guest_error_fd};
					}
					if (!entry->image.is_64)
						return native_result(raven::run_native_process_x86(modules, *entry, bootstrap.string(), timeout, launch, presentation, source),true,inherited_stdio);
					return native_result(raven::run_native_process(modules, *entry, bootstrap.string(), timeout, source, debug,presentation,launch),true,inherited_stdio);
				}
				return native_result(raven::run_native_modules(modules, *entry, bootstrap.string(), std::chrono::seconds(5)));
#else
				throw std::runtime_error("native execution requires an x86-64 Linux host");
#endif
			}
			const auto count = modules.bind_imports();
			std::cout << "Prepared " << modules.all().size() << " modules and bound " << count
				<< " imports (DLL initialization not implemented)\n";
			for (const auto &[name, module] : modules.all()) {
				for (const auto &symbol : module->imports) {
					uint64_t value = 0;
					for (size_t byte = 0; byte < (module->image.is_64 ? 8U : 4U); ++byte)
						value |= uint64_t(module->loaded.data()[symbol.iat_address + byte]) << (byte * 8);
					std::cout << name << ':' << symbol.module << '!'
						<< (symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name)
						<< " iat=0x" << std::hex << module->loaded.base() + symbol.iat_address
						<< " target=0x" << value << std::dec << '\n';
				}
			}
			return 0;
		}
		const auto bytes = read_file(argv[run_entry ? 3 : 2]);
		const raven::PEImage image(bytes);
		const auto imports = image.imports();
		if (run_entry) {
#ifdef RAVEN_NATIVE_CPU
			raven::LoadedImage loaded(image, image.image_base);
			const auto bootstrap = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "raven-bootstrap";
			return native_result(raven::run_native_entry(image, loaded, bootstrap.string(), std::chrono::seconds(5)));
#else
			throw std::runtime_error("native execution requires an x86-64 Linux host");
#endif
		}
		if (!dump_image)
			std::cout << (image.is_64 ? "PE64" : "PE32") << " base=0x" << std::hex << image.image_base
				<< " entry=0x" << image.entry << " size=0x" << image.image_size
				<< std::dec << " sections=" << image.sections.size() << " imports=" << imports.size() << '\n';
		if (mode == "--inspect-resources") {
			const auto key = [](const raven::ResourceKey &value) {
				if (const auto *id = std::get_if<uint32_t>(&value)) {
					std::cout << '#' << *id;
					return;
				}
				std::cout << '"';
				for (const auto character : std::get<std::u16string>(value)) {
					if (character >= 32 && character <= 126 && character != '"' && character != '\\')
						std::cout << static_cast<char>(character);
					else
						std::cout << "\\u" << std::hex << std::setw(4) << std::setfill('0')
							<< uint32_t(character) << std::dec << std::setfill(' ');
				}
				std::cout << '"';
			};
			for (const auto &resource : image.resources()) {
				std::cout << "Resource type=";
				key(resource.type);
				std::cout << " name=";
				key(resource.name);
				std::cout << " language=" << resource.language << " info=0x" << std::hex << resource.entry_address
					<< " data=0x" << resource.data_address << std::dec << " size=" << resource.size
					<< " codepage=" << resource.code_page << '\n';
			}
			return 0;
		}
		if (mode == "--inspect-delay-imports") {
			for (const auto &descriptor : image.delay_imports()) {
				std::cout << "Delay module=" << std::quoted(descriptor.module) << " descriptor=0x" << std::hex
					<< descriptor.descriptor_address << " handle=0x" << descriptor.module_handle_address
					<< " iat=0x" << descriptor.iat_address << " lookup=0x" << descriptor.lookup_address
					<< " bound=0x" << descriptor.bound_address << " unload=0x" << descriptor.unload_address
					<< " timestamp=0x" << descriptor.timestamp << std::dec << '\n';
				for (const auto &symbol : descriptor.imports)
					std::cout << descriptor.module << '!' << (symbol.by_ordinal ? "#" +
						std::to_string(symbol.ordinal) : symbol.name) << " iat=0x" << std::hex
						<< symbol.iat_address << std::dec << '\n';
			}
			return 0;
		}
		if (mode == "--inspect-exports") {
			for (const auto &symbol : image.exports()) {
				std::cout << symbol.ordinal << " 0x" << std::hex << symbol.address << std::dec
					<< ' ' << std::quoted(symbol.forwarder);
				for (const auto &name : symbol.names)
					std::cout << ' ' << std::quoted(name);
				std::cout << '\n';
			}
			return 0;
		}
		if (map_image) {
			uint64_t base = image.image_base;
			if (argc == 4) {
				size_t consumed = 0;
				base = std::stoull(argv[3], &consumed, 0);
				if (consumed != std::string(argv[3]).size())
					throw std::runtime_error("invalid image base argument");
			}
			raven::LoadedImage loaded(image, base);
			if (dump_image) {
				std::cout.write(reinterpret_cast<const char *>(loaded.data().data()),
					static_cast<std::streamsize>(loaded.data().size()));
				if (!std::cout)
					throw std::runtime_error("cannot write loaded image");
				return 0;
			}
			std::cout << "Shared image base=0x" << std::hex << loaded.base() << std::dec
				<< " bytes=" << loaded.data().size() << " relocations=" << loaded.relocation_count() << '\n';
			for (const auto &mapping : loaded.mappings())
				std::cout << "Mapping address=0x" << std::hex << mapping.address << " size=0x" << mapping.size
					<< " offset=0x" << mapping.file_offset << std::dec << " protection=" << mapping.protection << '\n';
			return 0;
		}
		std::map<std::string, size_t> modules;
		for (const auto &import : imports)
			++modules[import.module];
		if (std::string(argv[1]) == "--inspect-imports") {
			for (const auto &import : imports)
				std::cout << import.module << '!' << (import.by_ordinal ? "#" + std::to_string(import.ordinal) : import.name) << '\n';
		} else {
			for (const auto &[module, count] : modules)
				std::cout << module << ' ' << count << '\n';
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "raven: " << error.what() << '\n';
		return 1;
	}
}
