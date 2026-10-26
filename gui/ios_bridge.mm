#include "gui/ios_bridge.hpp"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/time/clock.hpp"
#include "elm327/controller/bridged_link.hpp"
#include "elm327/controller/controller.hpp"
#include "sim/demo.hpp"

namespace {

/// How long scan() looks for adapters.
constexpr double kScanSeconds{15.0};

/// scan() asks the Core Bluetooth side at most this often: the device list calls it every frame.
constexpr common::Duration kScanRequestInterval{std::chrono::seconds{1}};

/// Adapters kept from a scan at most.
constexpr NSUInteger kMaxFound{32U};

/// Bytes taken from the link per read: far more than a command.
constexpr std::size_t kReadSize{512U};

/// The parts of a name that make an advertising device an OBD adapter, when it does not advertise
/// one of the services of adapter_services().
NSArray<NSString*>* adapter_names() {
    return @[ @"OBD", @"ELM", @"LINK", @"Link", @"link", @"iCar", @"KONNWEI", @"Veepeak", @"Carista" ];
}

/// The GATT services of the usual adapters, with the characteristics they notify and take.
NSArray<CBUUID*>* adapter_services() {
    return @[
        [CBUUID UUIDWithString:@"FFF0"], [CBUUID UUIDWithString:@"FFE0"], [CBUUID UUIDWithString:@"18F0"],
        [CBUUID UUIDWithString:@"E7810A71-73AE-499D-8C15-FAA9AEF0C3F2"]
    ];
}

/// Writes the status byte @p answer on the bridge's end @p end of the link.
void answer(const int end, const elm327::LinkAnswer answer) {
    const std::uint8_t byte{static_cast<std::uint8_t>(answer)};
    const ssize_t written{::write(end, &byte, sizeof(byte))};
    static_cast<void>(written);  // The other side gave up already: it finds the link closed.
}

}  // namespace

/// Finds and connects Bluetooth Low Energy adapters, and copies between one of them and its end of a
/// bridged link. Everything runs on its queue.
@interface ElmBluetooth : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, strong) CBCentralManager* central;
@property(nonatomic, strong) NSMutableDictionary<NSUUID*, CBPeripheral*>* found;
@property(nonatomic) BOOL wantsScan;
@property(nonatomic) NSUInteger scanGeneration;
// The link: the adapter, its characteristics, the bridge's end of the socket pair and its reader.
@property(nonatomic, strong) CBPeripheral* peripheral;
@property(nonatomic, strong) CBCharacteristic* notifier;
@property(nonatomic, strong) CBCharacteristic* writer;
@property(nonatomic, strong) dispatch_source_t reader;
@property(nonatomic) int end;
@property(nonatomic) BOOL answered;
@property(nonatomic, copy) NSString* pendingIdentifier;
+ (ElmBluetooth*)shared;
@end

@implementation ElmBluetooth

+ (ElmBluetooth*)shared {
    static ElmBluetooth* shared{nil};
    static dispatch_once_t once{0};
    dispatch_once(&once, ^{
      shared = [[ElmBluetooth alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _queue = dispatch_queue_create("elm327.bluetooth", DISPATCH_QUEUE_SERIAL);
        _found = [NSMutableDictionary dictionary];
        _end = -1;
    }
    return self;
}

/// Creates the central manager on the first use, which asks the person for the permission. Queue.
- (void)start {
    if (self.central == nil) {
        self.central = [[CBCentralManager alloc] initWithDelegate:self
                                                            queue:self.queue
                                                          options:@{CBCentralManagerOptionShowPowerAlertKey : @NO}];
    }
}

- (void)scan {
    [self start];
    self.wantsScan = YES;
    if (self.central.state == CBManagerStatePoweredOn) {
        [self startScanning];
    }
}

- (void)startScanning {
    self.wantsScan = NO;
    if (!self.central.isScanning) {
        [self.central scanForPeripheralsWithServices:nil
                                             options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @NO}];
    }
    self.scanGeneration += 1U;
    const NSUInteger generation{self.scanGeneration};
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(kScanSeconds * NSEC_PER_SEC)),
                   self.queue, ^{
                     if ((generation == self.scanGeneration) && self.central.isScanning) {
                         [self.central stopScan];
                     }
                   });
}

- (elm327::gui::ios::BluetoothState)state {
    using elm327::gui::ios::BluetoothState;
    BluetoothState state{BluetoothState::kStarting};
    if (self.central != nil) {
        switch (self.central.state) {
            case CBManagerStatePoweredOn:
                state = BluetoothState::kReady;
                break;
            case CBManagerStatePoweredOff:
                state = BluetoothState::kOff;
                break;
            case CBManagerStateUnauthorized:
                state = BluetoothState::kNoPermission;
                break;
            case CBManagerStateUnsupported:
                state = BluetoothState::kUnsupported;
                break;
            case CBManagerStateUnknown:
            case CBManagerStateResetting:
                break;
        }
    }
    return state;
}

// MARK: The link.

/// Connects to the adapter @p identifier for the bridge's end @p end, after a link before it ends.
- (void)connect:(NSString*)identifier end:(int)end {
    [self closeLink];
    self.end = end;
    self.answered = NO;
    [self watchEnd];
    [self start];
    if (self.central.state == CBManagerStatePoweredOn) {
        [self connectTo:identifier];
    } else if ((self.central.state == CBManagerStateUnknown) || (self.central.state == CBManagerStateResetting)) {
        self.pendingIdentifier = identifier;  // centralManagerDidUpdateState: goes on.
    } else {
        [self refuse:[self answerForState]];
    }
}

- (elm327::LinkAnswer)answerForState {
    elm327::LinkAnswer result{elm327::LinkAnswer::kNotFound};
    if (self.central.state == CBManagerStatePoweredOff) {
        result = elm327::LinkAnswer::kOff;
    } else if (self.central.state == CBManagerStateUnauthorized) {
        result = elm327::LinkAnswer::kNoPermission;
    }
    return result;
}

- (void)connectTo:(NSString*)identifier {
    NSUUID* const uuid{[[NSUUID alloc] initWithUUIDString:identifier]};
    NSArray<CBPeripheral*>* const known{
        (uuid != nil) ? [self.central retrievePeripheralsWithIdentifiers:@[ uuid ]] : @[]};
    if (known.count == 0U) {
        [self refuse:elm327::LinkAnswer::kNotFound];
    } else {
        self.peripheral = known.firstObject;
        self.peripheral.delegate = self;
        [self.central connectPeripheral:self.peripheral options:nil];
    }
}

/// Reads what the native side sends on the link and writes it to the adapter; the native side
/// closing its end ends the link.
- (void)watchEnd {
    const int end{self.end};
    dispatch_source_t const source{
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(end), 0U, self.queue)};
    dispatch_source_set_event_handler(source, ^{
      std::array<std::uint8_t, kReadSize> buffer{};
      const ssize_t count{::read(end, buffer.data(), buffer.size())};
      if (count <= 0) {
          [self closeLink];
      } else {
          [self send:[NSData dataWithBytes:buffer.data() length:static_cast<NSUInteger>(count)]];
      }
    });
    dispatch_source_set_cancel_handler(source, ^{
      static_cast<void>(::close(end));
    });
    self.reader = source;
    dispatch_resume(source);
}

/// Writes @p data to the adapter, in pieces as long as it takes.
- (void)send:(NSData*)data {
    if ((self.peripheral != nil) && (self.writer != nil)) {
        const BOOL quiet{(self.writer.properties & CBCharacteristicPropertyWriteWithoutResponse) != 0U};
        const CBCharacteristicWriteType type{quiet ? CBCharacteristicWriteWithoutResponse
                                                   : CBCharacteristicWriteWithResponse};
        const NSUInteger piece{[self.peripheral maximumWriteValueLengthForType:type]};
        for (NSUInteger offset{0U}; offset < data.length; offset += piece) {
            const NSUInteger length{((data.length - offset) < piece) ? (data.length - offset) : piece};
            [self.peripheral writeValue:[data subdataWithRange:NSMakeRange(offset, length)]
                      forCharacteristic:self.writer
                                   type:type];
        }
    }
}

/// Answers @p reason on the link unless it was answered, and ends it.
- (void)refuse:(elm327::LinkAnswer)reason {
    if ((!self.answered) && (self.end >= 0)) {
        answer(self.end, reason);
        self.answered = YES;
    }
    [self closeLink];
}

/// Ends the link: the Bluetooth connection, and the bridge's end (closed by the reader's cancel).
- (void)closeLink {
    if (self.peripheral != nil) {
        self.peripheral.delegate = nil;
        [self.central cancelPeripheralConnection:self.peripheral];
    }
    self.peripheral = nil;
    self.notifier = nil;
    self.writer = nil;
    self.pendingIdentifier = nil;
    if (self.reader != nil) {
        dispatch_source_cancel(self.reader);
        self.reader = nil;
    } else if (self.end >= 0) {
        static_cast<void>(::close(self.end));
    }
    self.end = -1;
}

// MARK: CBCentralManagerDelegate.

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
    if (central.state == CBManagerStatePoweredOn) {
        if (self.wantsScan) {
            [self startScanning];
        }
        if (self.pendingIdentifier != nil) {
            NSString* const identifier{self.pendingIdentifier};
            self.pendingIdentifier = nil;
            [self connectTo:identifier];
        }
    } else if ((central.state != CBManagerStateUnknown) && (central.state != CBManagerStateResetting)) {
        if ((self.pendingIdentifier != nil) || (self.peripheral != nil)) {
            [self refuse:[self answerForState]];
        }
    } else {
        // Wait for the next state.
    }
}

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
        advertisementData:(NSDictionary<NSString*, id>*)advertisement
                     RSSI:(NSNumber*)rssi {
    static_cast<void>(central);
    static_cast<void>(rssi);
    NSArray<CBUUID*>* const services{advertisement[CBAdvertisementDataServiceUUIDsKey]};
    BOOL adapter{NO};
    for (CBUUID* service in services) {
        adapter = adapter || [adapter_services() containsObject:service];
    }
    NSString* const name{peripheral.name};
    for (NSString* part in adapter_names()) {
        adapter = adapter || ((name != nil) && ([name rangeOfString:part].location != NSNotFound));
    }
    if (adapter && ((self.found.count < kMaxFound) || (self.found[peripheral.identifier] != nil))) {
        self.found[peripheral.identifier] = peripheral;
    }
}

- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {
    static_cast<void>(central);
    [peripheral discoverServices:nil];
}

- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
                         error:(NSError*)error {
    static_cast<void>(central);
    static_cast<void>(peripheral);
    static_cast<void>(error);
    [self refuse:elm327::LinkAnswer::kRefused];
}

- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
                      error:(NSError*)error {
    static_cast<void>(central);
    static_cast<void>(error);
    if (peripheral == self.peripheral) {
        [self refuse:elm327::LinkAnswer::kRefused];  // Before the answer: refused; after: gone.
    }
}

// MARK: CBPeripheralDelegate.

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
    if ((error != nil) || (peripheral.services.count == 0U)) {
        [self refuse:elm327::LinkAnswer::kRefused];
    } else {
        for (CBService* service in peripheral.services) {
            [peripheral discoverCharacteristics:nil forService:service];
        }
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverCharacteristicsForService:(CBService*)service
                                   error:(NSError*)error {
    if ((error != nil) || (self.notifier != nil)) {
        return;  // This service has nothing, or the link has its characteristics already.
    }
    // The first characteristic of the service that notifies (or indicates), and the first that takes
    // writes: of the usual services they are the adapter's.
    CBCharacteristic* notifier{nil};
    CBCharacteristic* writer{nil};
    for (CBCharacteristic* characteristic in service.characteristics) {
        const CBCharacteristicProperties properties{characteristic.properties};
        if ((notifier == nil) &&
            ((properties & (CBCharacteristicPropertyNotify | CBCharacteristicPropertyIndicate)) != 0U)) {
            notifier = characteristic;
        }
        if ((writer == nil) &&
            ((properties & (CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse)) != 0U)) {
            writer = characteristic;
        }
    }
    if ((notifier != nil) && (writer != nil)) {
        self.notifier = notifier;
        self.writer = writer;
        [peripheral setNotifyValue:YES forCharacteristic:notifier];
    } else if (service == peripheral.services.lastObject) {
        [self refuse:elm327::LinkAnswer::kRefused];  // No service had both.
    } else {
        // Another service may have them.
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
                                          error:(NSError*)error {
    static_cast<void>(peripheral);
    if (characteristic != self.notifier) {
        return;
    }
    if ((error != nil) || (!characteristic.isNotifying)) {
        [self refuse:elm327::LinkAnswer::kRefused];
    } else if (!self.answered) {
        answer(self.end, elm327::LinkAnswer::kConnected);
        self.answered = YES;
    } else {
        // Already linked.
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
                              error:(NSError*)error {
    static_cast<void>(peripheral);
    if ((error == nil) && (characteristic == self.notifier) && self.answered && (self.end >= 0)) {
        NSData* const value{characteristic.value};
        const ssize_t written{::write(self.end, value.bytes, value.length)};
        static_cast<void>(written);  // The native side reads promptly; a full buffer loses a reply.
    }
}

@end

namespace elm327 {
namespace gui {
namespace ios {
namespace {

/// The LinkHandOver of open_bridged_link(): hands @p end to the Core Bluetooth side, which connects
/// to the adapter @p address.
void hand_to_core_bluetooth(const char* const address, const common::NativeHandle end, void* const /*context*/) noexcept {
    NSString* const identifier{@(address)};
    ElmBluetooth* const bluetooth{[ElmBluetooth shared]};
    dispatch_async(bluetooth.queue, ^{
      [bluetooth connect:identifier end:end];
    });
}

/// Copies @p text into @p out. @return false if it does not fit or there is none (@p out is empty).
template <std::size_t Capacity>
bool copy_text(NSString* const text, common::StaticString<Capacity>& out) noexcept {
    out.clear();
    const char* const characters{(text != nil) ? text.UTF8String : nullptr};
    const bool copied{(characters != nullptr) && out.append(std::string_view{characters})};
    if (!copied) {
        out.clear();
    }
    return copied;
}

}  // namespace

void scan() noexcept {
    static std::atomic<std::int64_t> last_request{0};  // In ns of MonotonicClock.
    const std::int64_t now{common::MonotonicClock::now().time_since_epoch().count()};
    if ((now - last_request.load(std::memory_order_relaxed)) >= kScanRequestInterval.count()) {
        last_request.store(now, std::memory_order_relaxed);
        ElmBluetooth* const bluetooth{[ElmBluetooth shared]};
        dispatch_async(bluetooth.queue, ^{
          [bluetooth scan];
        });
    }
}

BluetoothState found_devices(const common::Span<FoundDevice> devices, std::size_t& count) noexcept {
    ElmBluetooth* const bluetooth{[ElmBluetooth shared]};
    __block BluetoothState state{BluetoothState::kStarting};
    __block NSArray<CBPeripheral*>* found{@[]};
    dispatch_sync(bluetooth.queue, ^{
      state = [bluetooth state];
      found = bluetooth.found.allValues;
    });
    count = 0U;
    for (CBPeripheral* peripheral in found) {
        if (count < devices.size()) {
            FoundDevice& device{devices[count]};
            if (copy_text(peripheral.identifier.UUIDString, device.identifier)) {
                if ((!copy_text(peripheral.name, device.name)) || device.name.empty()) {
                    static_cast<void>(device.name.assign(device.identifier.view()));  // Cannot fail: shorter.
                }
                ++count;
            }
        }
    }
    return state;
}

common::Status documents_directory(Path& directory) noexcept {
    NSString* const documents{
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject};
    directory.clear();
    const char* const path{(documents != nil) ? documents.fileSystemRepresentation : nullptr};
    return ((path != nullptr) && directory.append(std::string_view{path})) ? common::Status::kOk
                                                                           : common::Status::kNotFound;
}

common::Result<common::NativeHandle> open_link(const char* const device, const common::Deadline& deadline,
                                               const common::NativeHandle cancel, void* const /*context*/) noexcept {
    using LinkResult = common::Result<common::NativeHandle>;
    const std::string_view name{device};
    LinkResult result{LinkResult::failure(common::Status::kNotSupported)};
    if (name == kSimulatorDevice) {
        const int stream{sim::start_demo()};
        result = (stream >= 0) ? LinkResult::success(stream) : LinkResult::failure(common::Status::kResourceLimit);
    } else if ((name.size() > kBluetoothLowEnergyPrefix.size()) &&
               (std::string_view{name.data(), kBluetoothLowEnergyPrefix.size()} == kBluetoothLowEnergyPrefix)) {
        // The identifier follows the prefix, up to the end of the device.
        result = open_bridged_link(&device[kBluetoothLowEnergyPrefix.size()], &hand_to_core_bluetooth, nullptr,
                                   deadline, cancel);
    } else {
        // Wi-Fi ("tcp:"), which the Adapter opens, or a typed device.
    }
    return result;
}

}  // namespace ios
}  // namespace gui
}  // namespace elm327
