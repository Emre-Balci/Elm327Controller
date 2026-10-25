#include <cstdint>
#include <string_view>

#include "elm327/obd/service.hpp"
#include "test_framework.hpp"

namespace {

elm327::Access access_of(const std::string_view command) {
    return elm327::command_access(command);
}

}  // namespace

TEST_CASE(service_obd_classification) {
    using elm327::Access;
    // The OBD-II reads.
    for (const unsigned read : {0x01U, 0x02U, 0x03U, 0x05U, 0x06U, 0x07U, 0x09U, 0x0AU}) {
        CHECK(elm327::obd_service_access(static_cast<std::uint8_t>(read)) == Access::kRead);
    }
    // The OBD-II writes: clear the DTCs, and control an on-board system.
    CHECK(elm327::obd_service_access(0x04U) == Access::kWrite);
    CHECK(elm327::obd_service_access(0x08U) == Access::kWrite);
    // Not an OBD-II mode.
    CHECK(elm327::obd_service_access(0x00U) == Access::kUnknown);
    CHECK(elm327::obd_service_access(0x22U) == Access::kUnknown);
}

TEST_CASE(service_uds_classification) {
    using elm327::Access;
    // UDS/KWP reads.
    for (const unsigned read : {0x17U, 0x18U, 0x19U, 0x1AU, 0x21U, 0x22U, 0x23U, 0x24U, 0x2AU, 0x3EU}) {
        CHECK(elm327::uds_service_access(static_cast<std::uint8_t>(read)) == Access::kRead);
    }
    // UDS/KWP writes, the heart of expert mode: they must never be mistaken for reads.
    for (const unsigned write : {0x10U, 0x11U, 0x14U, 0x27U, 0x28U, 0x2CU, 0x2EU, 0x2FU, 0x30U, 0x31U, 0x34U,
                                     0x35U, 0x36U, 0x37U, 0x3BU, 0x3DU, 0x85U}) {
        CHECK(elm327::uds_service_access(static_cast<std::uint8_t>(write)) == Access::kWrite);
    }
    // SecurityAccess is a write even for the odd "request seed" subfunction: its purpose is to unlock.
    CHECK(elm327::uds_service_access(0x27U) == Access::kWrite);
    CHECK(elm327::uds_service_access(0x55U) == Access::kUnknown);
}

TEST_CASE(service_combined_is_safety_first) {
    using elm327::Access;
    // A write in either protocol wins.
    CHECK(elm327::service_access(0x04U) == Access::kWrite);  // OBD clear.
    CHECK(elm327::service_access(0x2EU) == Access::kWrite);  // UDS write.
    CHECK(elm327::service_access(0x01U) == Access::kRead);   // OBD current data.
    CHECK(elm327::service_access(0x22U) == Access::kRead);   // UDS read DID.
    CHECK(elm327::service_access(0xFFU) == Access::kUnknown);
}

TEST_CASE(service_command_access_at_commands) {
    using elm327::Access;
    // AT commands configure the adapter, never the vehicle: always a read, in either case.
    CHECK(access_of("ATZ") == Access::kRead);
    CHECK(access_of("atz") == Access::kRead);
    CHECK(access_of("ATRV") == Access::kRead);
    CHECK(access_of("ATSP6") == Access::kRead);
    CHECK(access_of("ATSH7E0") == Access::kRead);
    CHECK(access_of("AT E0") == Access::kRead);
}

TEST_CASE(service_command_access_obd_requests) {
    using elm327::Access;
    CHECK(access_of("010C") == Access::kRead);   // Read engine speed.
    CHECK(access_of("03") == Access::kRead);      // Stored DTCs.
    CHECK(access_of("0902") == Access::kRead);    // VIN.
    CHECK(access_of("22F190") == Access::kRead);  // UDS read DID.
    CHECK(access_of("3E80") == Access::kRead);    // Tester present.

    CHECK(access_of("04") == Access::kWrite);           // Clear the DTCs.
    CHECK(access_of("2EF19057") == Access::kWrite);     // UDS write DID.
    CHECK(access_of("3101FF00") == Access::kWrite);     // Routine control.
    CHECK(access_of("2701") == Access::kWrite);         // Security access.
}

TEST_CASE(service_command_access_is_conservative) {
    using elm327::Access;
    // Spaces between or around the digits are ignored, so a write cannot be hidden with them.
    CHECK(access_of("0 4") == Access::kWrite);
    CHECK(access_of("  04  ") == Access::kWrite);
    CHECK(access_of(" 2E F1 90 57") == Access::kWrite);
    CHECK(access_of(" 01 0C ") == Access::kRead);

    // Anything that is not an AT command and not a hexadecimal request is unknown, so it is refused.
    CHECK(access_of("") == Access::kUnknown);
    CHECK(access_of("   ") == Access::kUnknown);
    CHECK(access_of("STPX") == Access::kUnknown);  // An STN command could transmit a frame.
    CHECK(access_of("G0") == Access::kUnknown);
    CHECK(access_of("4") == Access::kUnknown);  // A single digit is not a full service byte.
}

TEST_CASE(service_gate_blocks_only_in_read_only) {
    using elm327::AccessPolicy;
    // In read-write mode nothing is blocked.
    CHECK(!elm327::command_blocked("04", AccessPolicy::kReadWrite));
    CHECK(!elm327::command_blocked("2EF190", AccessPolicy::kReadWrite));
    CHECK(!elm327::command_blocked("STPX", AccessPolicy::kReadWrite));

    // In read-only mode, only pure reads (and AT commands) pass; writes and unknowns are blocked.
    CHECK(!elm327::command_blocked("010C", AccessPolicy::kReadOnly));
    CHECK(!elm327::command_blocked("ATZ", AccessPolicy::kReadOnly));
    CHECK(!elm327::command_blocked("22F190", AccessPolicy::kReadOnly));
    CHECK(elm327::command_blocked("04", AccessPolicy::kReadOnly));
    CHECK(elm327::command_blocked("2EF190", AccessPolicy::kReadOnly));
    CHECK(elm327::command_blocked("STPX", AccessPolicy::kReadOnly));
    CHECK(elm327::command_blocked("", AccessPolicy::kReadOnly));
}

TEST_CASE(service_access_names) {
    CHECK(std::string_view{elm327::to_string(elm327::Access::kRead)} == "read");
    CHECK(std::string_view{elm327::to_string(elm327::Access::kWrite)} == "write");
    CHECK(std::string_view{elm327::to_string(elm327::Access::kUnknown)} == "unknown");
}
