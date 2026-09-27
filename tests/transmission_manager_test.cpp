#include "transmission_manager.h"

#include <iostream>
#include <memory>
#include <string>

int main() {
  TransmissionManager transmission;
  int failures = 0;

  auto expect = [&failures](bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << std::endl;
      ++failures;
    }
  };

  expect(transmission.GetActiveConnectionCount() == 0,
         "initial active connection count is zero");

  {
    TransmissionManager callback_transmission;
    int starts = 0;
    int ends = 0;
    callback_transmission.SetRemoteControlSessionCallback(
        [&](const std::string& transmission_id, const std::string& host_id,
            const std::string& guest_id, bool started) {
          if (transmission_id == "host-callback" &&
              host_id == "host-callback" && guest_id == "guest-callback") {
            if (started) {
              ++starts;
            } else {
              ++ends;
            }
          }
        });
    expect(callback_transmission.BindHostToTransmission("host-callback",
                                                        "host-callback"),
           "callback host binds to transmission");
    expect(callback_transmission.BindGuestToTransmission("guest-callback",
                                                         "host-callback"),
           "callback guest joins transmission");
    expect(starts == 1, "guest join emits remote control start callback");
    expect(callback_transmission.ReleaseGuestFromTransmission("guest-callback"),
           "callback guest leaves transmission");
    expect(ends == 1, "guest leave emits remote control end callback");
  }

  expect(!transmission.BindGuestToTransmission("orphan-guest", "missing-host"),
         "guest cannot join a missing host transmission");
  expect(transmission.GetActiveConnectionCount() == 0,
         "missing host join is ignored by active connection count");

  expect(transmission.BindHostToTransmission("B", "B"),
         "host B binds to transmission B");
  auto waiting = transmission.GetTransmissionSnapshots();
  expect(waiting.size() == 1 && !waiting[0].active,
         "host-only transmission remains available but is inactive");
  size_t active_count = 0;
  expect(transmission.GetTransmissionSnapshots(10, 0, "", &active_count, true)
                 .empty() && active_count == 0,
         "active snapshots exclude a host waiting for a controller");
  expect(transmission.BindGuestToTransmission("A", "B"),
         "guest A joins transmission B");
  expect(transmission.GetActiveConnectionCount() == 1,
         "guest join increments active connection count once");

  auto snapshots = transmission.GetTransmissionSnapshots();
  expect(snapshots.size() == 1, "snapshot contains one active transmission");
  expect(snapshots[0].transmission_id == "B", "snapshot has transmission id");
  expect(snapshots[0].host_id == "B", "snapshot has host id");
  expect(snapshots[0].guest_ids.size() == 1 &&
             snapshots[0].guest_ids[0] == "A",
         "snapshot has guest id");
  expect(snapshots[0].participant_count == 2,
         "snapshot counts host plus guest");
  expect(snapshots[0].active, "snapshot marks active transmission");

  expect(transmission.DisconnectTransmission("B"),
         "disconnect existing transmission succeeds");
  expect(transmission.GetActiveConnectionCount() == 0,
         "disconnect decrements active connection count");
  expect(transmission.GetTransmissionSnapshots().empty(),
         "disconnect removes transmission snapshot");
  expect(transmission.DisconnectTransmission("B"),
         "disconnect missing transmission is idempotent");

  expect(transmission.BindHostToTransmission("B", "B"),
         "host B rebinds to transmission B");
  expect(transmission.BindGuestToTransmission("A", "B"),
         "guest A rejoins transmission B");
  expect(transmission.GetActiveConnectionCount() == 1,
         "guest rejoin increments active connection count once");

  expect(!transmission.BindGuestToTransmission("B", "B"),
         "host B is not counted as a guest of its own transmission");
  expect(transmission.GetActiveConnectionCount() == 1,
         "host offer path does not increment active connection count");

  expect(transmission.ReleaseGuestFromTransmission("A"),
         "guest A leaves transmission B");
  expect(transmission.GetActiveConnectionCount() == 0,
         "guest leave decrements active connection count to zero");
  waiting = transmission.GetTransmissionSnapshots();
  expect(waiting.size() == 1 && !waiting[0].active,
         "transmission becomes inactive after its final controller leaves");
  expect(transmission.GetTransmissionSnapshots(10, 0, "", &active_count, true)
                 .empty() && active_count == 0,
         "ended control session is omitted from active snapshots");

  expect(transmission.ReleaseTransmission("B"), "host B releases transmission B");
  expect(transmission.GetActiveConnectionCount() == 0,
         "host close does not decrement an already released guest count");

  {
    TransmissionManager session_transmission;
    auto guest_connection = std::make_shared<int>(1);
    websocketpp::connection_hdl guest_hdl(guest_connection);

    expect(session_transmission.BindUserToWsHandle("guest-A", guest_hdl),
           "guest A binds to websocket handle");
    expect(session_transmission.BindHostToTransmission("guest-A", "guest-A"),
           "guest A has its own host transmission");
    expect(session_transmission.BindHostToTransmission("host-B", "host-B"),
           "host B has its own host transmission");
    expect(session_transmission.BindGuestToTransmission("guest-A", "host-B"),
           "guest A joins host B transmission");
    expect(session_transmission.GetActiveConnectionCount() == 1,
           "host B has one guest connection");

    expect(session_transmission.ReleaseUserSession(guest_hdl) == "guest-A",
           "guest A websocket session releases");
    expect(session_transmission.GetActiveConnectionCount() == 0,
           "guest disconnect removes host-side guest connection");

    auto remaining = session_transmission.GetTransmissionSnapshots();
    expect(remaining.size() == 1 && remaining[0].transmission_id == "host-B",
           "guest A own host transmission is released");
    expect(remaining.size() == 1 && remaining[0].guest_ids.empty(),
           "host B no longer lists disconnected guest A");
  }

  {
    TransmissionManager prune_transmission;
    int ends = 0;
    prune_transmission.SetRemoteControlSessionCallback(
        [&](const std::string&, const std::string&, const std::string&,
            bool started) {
          if (!started) {
            ++ends;
          }
        });

    auto live_host_connection = std::make_shared<int>(1);
    auto live_guest_connection = std::make_shared<int>(2);
    websocketpp::connection_hdl live_host_hdl(live_host_connection);
    websocketpp::connection_hdl live_guest_hdl(live_guest_connection);

    prune_transmission.BindHostToTransmission("offline-host", "tx-offline");
    prune_transmission.BindGuestToTransmission("offline-guest", "tx-offline");
    prune_transmission.BindHostToTransmission("live-host", "tx-live");
    prune_transmission.BindGuestToTransmission("live-guest", "tx-live");
    prune_transmission.BindUserToWsHandle("live-host", live_host_hdl);
    prune_transmission.BindUserToWsHandle("live-guest", live_guest_hdl);

    expect(prune_transmission.PruneDisconnectedTransmissions() == 1,
           "prune removes disconnected restored guest connection");
    expect(ends == 1, "prune emits remote control end callback");
    expect(!prune_transmission.IsTransmissionExist("tx-offline"),
           "prune removes disconnected host transmission");
    expect(prune_transmission.IsTransmissionExist("tx-live"),
           "prune keeps transmission with connected participants");
    expect(prune_transmission.GetActiveConnectionCount() == 1,
           "prune leaves live active connection count intact");
  }

  {
    TransmissionManager duplicate_transmission;
    auto first_connection = std::make_shared<int>(1);
    auto second_connection = std::make_shared<int>(2);
    websocketpp::connection_hdl first_hdl(first_connection);
    websocketpp::connection_hdl second_hdl(second_connection);

    expect(duplicate_transmission.BindUserToWsHandle("device-1", first_hdl),
           "device binds first websocket handle");
    expect(duplicate_transmission.BindHostToTransmission("device-1", "device-1"),
           "device has a host transmission");
    expect(duplicate_transmission.BindUserToWsHandle("device-1", second_hdl),
           "device binds second websocket handle");

    expect(duplicate_transmission.ReleaseUserSession(second_hdl).empty(),
           "closing one duplicate websocket does not log out device");
    expect(duplicate_transmission.IsTransmissionExist("device-1"),
           "device host transmission remains while another websocket is alive");
    expect(duplicate_transmission.GetUserId(
               duplicate_transmission.GetWsHandle("device-1")) == "device-1",
           "device websocket handle falls back to remaining connection");

    expect(duplicate_transmission.ReleaseUserSession(first_hdl) == "device-1",
           "closing final duplicate websocket logs out device");
    expect(!duplicate_transmission.IsTransmissionExist("device-1"),
           "device host transmission is released after final websocket closes");
  }

  {
    TransmissionManager paged_transmission;
    paged_transmission.BindHostToTransmission("host-1", "tx-1");
    paged_transmission.BindGuestToTransmission("guest-1", "tx-1");
    paged_transmission.BindHostToTransmission("host-2", "tx-2");
    paged_transmission.BindGuestToTransmission("guest-2", "tx-2");

    size_t filtered_count = 0;
    auto page =
        paged_transmission.GetTransmissionSnapshots(1, 1, "", &filtered_count);
    expect(filtered_count == 2,
           "paged snapshot reports filtered transmission count");
    expect(page.size() == 1, "paged snapshot applies limit and offset");

    auto filtered = paged_transmission.GetTransmissionSnapshots(
        10, 0, "guest-2", &filtered_count);
    expect(filtered_count == 1, "paged snapshot count supports search");
    expect(filtered.size() == 1 && filtered[0].transmission_id == "tx-2",
           "paged snapshot search matches guest id");
  }

  return failures == 0 ? 0 : 1;
}
