#include "common.h"
#include "conceal.h"
#include "file_utils.h"
#include "program_args.h"
#include "recover.h"
#include "process_hardening.h"
#include "signal_utils.h"

#include <iostream>
#include <print>
#include <stdexcept>

namespace {
[[nodiscard]] int run(int argc, char** argv) {
    throwIf(sodium_init() < 0, "Libsodium initialization failed!");

    auto args_opt = ProgramArgs::parse(argc, argv);
    if (!args_opt) return 0;

    const auto& args = *args_opt;
    switch (args.mode) {
        case Mode::conceal: {
            vBytes jpg_vec = readFile(args.image_file_path, coverCheckFor(args.option));
            concealData(jpg_vec, args.option, args.image_file_path, args.data_file_path);
            return 0;
        }
        case Mode::recover:
            recoverData(args.image_file_path);
            return 0;
        case Mode::capsize: {
            vBytes jpg_vec = readFile(args.image_file_path, coverCheckFor(args.option));
            displayCapacity(jpg_vec, args.option);
            return 0;
        }
        default:
            throw std::runtime_error("Internal Error: Unsupported mode.");
    }
}
} // namespace

int main(int argc, char** argv) {
    disableCoreDumps();
    try {
        installProcessSignalHandlers();
        return run(argc, argv);
    } catch (const SignalCancellation& interruption) {
        reraiseSignalAfterCleanup(interruption.signalNumber());
    } catch (const std::exception& e) {
        // A write the signal interrupted fails as an ordinary error (std::print
        // throws on a short write): still end by the signal, writing nothing
        // more. Under `stty tostop` a message from a background job would stop
        // it on SIGTTOU with the signal already spent.
        if (const int signal_number = pendingSignalCancellation(); signal_number != 0)
            reraiseSignalAfterCleanup(signal_number);
        std::println(std::cerr, "\n{}\n", terminalSafeMessage(e.what()));
        return 1;
    } catch (...) {
        if (const int signal_number = pendingSignalCancellation(); signal_number != 0)
            reraiseSignalAfterCleanup(signal_number);
        std::println(std::cerr, "\nUnknown fatal error.\n");
        return 1;
    }
}
