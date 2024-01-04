#include "net_utilities.h"

#include "core/object_data.h"
#include "modules/network_synchronizer/net_utilities.h"
#include "modules/network_synchronizer/networked_controller.h"
#include "modules/network_synchronizer/scene_synchronizer_debugger.h"
#include "networked_controller.h"
#include "scene_synchronizer.h"
#include "scene_synchronizer_debugger.h"
#include <limits>
#include <memory>

void NS::PeerData::set_latency(int p_latency) {
	compressed_latency = std::round(float(std::min(p_latency, 1000)) / 4.0);
}

int NS::PeerData::get_latency() const {
	return int(compressed_latency) * 4;
}

void NS::PeerData::make_controller(NS::SceneSynchronizerBase &p_scene_sync, int p_peer) {
	controller = std::make_unique<NS::NetworkedControllerBase>();
	controller->setup_synchronizer(p_scene_sync, p_peer);
}

bool NS::SyncGroup::is_realtime_node_list_changed() const {
	return simulated_sync_objects_list_changed;
}

bool NS::SyncGroup::is_trickled_node_list_changed() const {
	return trickled_sync_objects_list_changed;
}

const std::vector<int> NS::SyncGroup::get_peers_with_newly_calculated_latency() const {
	return peers_with_newly_calculated_latency;
}

const LocalVector<NS::SyncGroup::SimulatedObjectInfo> &NS::SyncGroup::get_simulated_sync_objects() const {
	return simulated_sync_objects;
}

const LocalVector<NS::SyncGroup::TrickledObjectInfo> &NS::SyncGroup::get_trickled_sync_objects() const {
	return trickled_sync_objects;
}

LocalVector<NS::SyncGroup::TrickledObjectInfo> &NS::SyncGroup::get_trickled_sync_objects() {
	return trickled_sync_objects;
}

void NS::SyncGroup::mark_changes_as_notified() {
	for (int i = 0; i < int(simulated_sync_objects.size()); ++i) {
		simulated_sync_objects[i].change.unknown = false;
		simulated_sync_objects[i].change.uknown_vars.clear();
		simulated_sync_objects[i].change.vars.clear();
	}
	for (int i = 0; i < int(trickled_sync_objects.size()); ++i) {
		trickled_sync_objects[i]._unknown = false;
	}
	simulated_sync_objects_list_changed = false;
	trickled_sync_objects_list_changed = false;
	peers_with_newly_calculated_latency.clear();
}

void NS::SyncGroup::add_listening_peer(int p_peer) {
	NS::VecFunc::insert_unique(listening_peers, p_peer);
	if (NS::VecFunc::insert_unique(networked_peers, p_peer)) {
		NS::VecFunc::insert_unique(peers_with_newly_calculated_latency, p_peer);
	}

	// Make all the controlled objects as simulated.
	const std::vector<ObjectData *> *controlled_objects_ptr = scene_sync->get_peer_controlled_objects_data(p_peer);
	if (controlled_objects_ptr) {
		for (ObjectData *od : (*controlled_objects_ptr)) {
			add_new_sync_object(od, true);
		}
	}

	notify_controllers_about_simulating_peer(p_peer, true);
}

void NS::SyncGroup::remove_listening_peer(int p_peer) {
	NS::VecFunc::remove_unordered(listening_peers, p_peer);

	// Remove all the controlled objects.
	const std::vector<ObjectData *> *controlled_objects_ptr = scene_sync->get_peer_controlled_objects_data(p_peer);
	if (controlled_objects_ptr) {
		for (ObjectData *od : (*controlled_objects_ptr)) {
			remove_sync_object(*od);
		}
	}

	notify_controllers_about_simulating_peer(p_peer, false);
}

uint32_t NS::SyncGroup::add_new_sync_object(ObjectData *p_object_data, bool p_is_simulated) {
	if (p_object_data->get_controlled_by_peer() != -1) {
		// This is a controller with an associated peer, update the networked_peer list.
		// Regardless if it's simulated or not.
		const int peer = p_object_data->get_controlled_by_peer();
		if (NS::VecFunc::insert_unique(networked_peers, peer)) {
			NS::VecFunc::insert_unique(peers_with_newly_calculated_latency, peer);
		}

		const bool is_listener_controlled = NS::VecFunc::has(listening_peers, peer);
		if (is_listener_controlled && !p_is_simulated) {
			// This object is controlled by a listener, so it's always simulated.
			p_is_simulated = true;
			SceneSynchronizerDebugger::singleton()->print(NS::WARNING, "The object `" + p_object_data->object_name + "` is being controlled by a peer listening to this sync group, so it can't be made non simulating.");
		}
	}

	if (p_is_simulated) {
		// Make sure the node is not contained into the trickled sync.
		const int tso_index = trickled_sync_objects.find(p_object_data);
		if (tso_index >= 0) {
			remove_sync_object(tso_index, false);
		}

		// Add it into the realtime sync nodes
		int index = simulated_sync_objects.find(p_object_data);

		if (index <= -1) {
			index = simulated_sync_objects.size();
			simulated_sync_objects.push_back(p_object_data);
			simulated_sync_objects_list_changed = true;

			SimulatedObjectInfo &info = simulated_sync_objects[index];

			info.change.unknown = true;

			for (int i = 0; i < int(p_object_data->vars.size()); ++i) {
				notify_new_variable(p_object_data, p_object_data->vars[i].var.name);
			}

			if (p_object_data->get_controlled_by_peer() > 0) {
				notify_controller_about_simulating_peers(p_object_data->get_controlled_by_peer(), true);
			}
		}

		return index;
	} else {
		// Make sure the node is not contained into the realtime sync.
		const int rsn_index = simulated_sync_objects.find(p_object_data);
		if (rsn_index >= 0) {
			remove_sync_object(rsn_index, true);
		}

		// Add it into the trickled sync nodes
		int index = trickled_sync_objects.find(p_object_data);

		if (index <= -1) {
			index = trickled_sync_objects.size();
			trickled_sync_objects.push_back(p_object_data);
			trickled_sync_objects[index]._unknown = true;
			trickled_sync_objects_list_changed = true;
		}

		return index;
	}
}

void NS::SyncGroup::remove_sync_object(std::size_t p_index, bool p_is_simulated) {
	int associted_peer = 0;

	if (p_is_simulated) {
		if (simulated_sync_objects[p_index].od->get_controlled_by_peer() > 0) {
			associted_peer = simulated_sync_objects[p_index].od->get_controlled_by_peer();
		}
	} else {
		if (trickled_sync_objects[p_index].od->get_controlled_by_peer() > 0) {
			associted_peer = trickled_sync_objects[p_index].od->get_controlled_by_peer();
		}
	}

	const bool is_listener_controlled = NS::VecFunc::has(listening_peers, associted_peer);
	ENSURE_MSG(!is_listener_controlled, "It's not possible to remove an object controlled by a listening peer from sync group.");

	if (p_is_simulated) {
		simulated_sync_objects.remove_at_unordered(p_index);
		simulated_sync_objects_list_changed = true;
	} else {
		trickled_sync_objects.remove_at_unordered(p_index);
		trickled_sync_objects_list_changed = true;
	}

	if (associted_peer > 0) {
		// If no other simulated objects controlled by `associated_peer` remove it from
		bool is_simulating = false;
		bool is_networking = false;

		for (auto &soi : simulated_sync_objects) {
			if (soi.od->get_controlled_by_peer() == associted_peer) {
				is_networking = true;
				is_simulating = true;
				break;
			}
		}

		if (!is_networking) {
			if (NS::VecFunc::has(listening_peers, associted_peer)) {
				is_networking = true;
			}
		}

		if (!is_networking) {
			for (auto &toi : trickled_sync_objects) {
				if (toi.od->get_controlled_by_peer() == associted_peer) {
					is_networking = true;
					break;
				}
			}
		}

		if (!is_simulating) {
			// No other objects associated to this peer are simulated, so notify the peer as non simulating.
			notify_controller_about_simulating_peers(associted_peer, false);
		}

		if (!is_networking) {
			NS::VecFunc::remove_unordered(networked_peers, associted_peer);
			NS::VecFunc::remove_unordered(peers_with_newly_calculated_latency, associted_peer);
		}
	}
}

void NS::SyncGroup::remove_sync_object(const ObjectData &p_object_data) {
	{
		const std::size_t index = find_simulated(p_object_data);
		if (index != std::numeric_limits<std::size_t>::max()) {
			remove_sync_object(index, true);
			// No need to check the trickled array. Nodes can be in 1 single array.
			return;
		}
	}

	{
		const std::size_t index = find_trickled(p_object_data);
		if (index != std::numeric_limits<std::size_t>::max()) {
			remove_sync_object(index, false);
		}
	}
}

template <class T>
void replace_nodes_impl(
		NS::SyncGroup &p_sync_group,
		LocalVector<T> &&p_nodes_to_add,
		bool p_is_simulated,
		LocalVector<T> &r_sync_group_nodes) {
	for (int i = int(r_sync_group_nodes.size()) - 1; i >= 0; i--) {
		const int64_t nta_index = p_nodes_to_add.find(r_sync_group_nodes[i].od);
		if (nta_index == -1) {
			// This node is not part of this sync group, remove it.
			p_sync_group.remove_sync_object(i, p_is_simulated);
		} else {
			// This node is still part of this SyncGroup.
			// Update the existing one.
			r_sync_group_nodes[i].update_from(p_nodes_to_add[nta_index]);

			// Then, make sure not to add again:
			p_nodes_to_add.remove_at_unordered(nta_index);

#ifdef DEBUG_ENABLED
			// Make sure there are no duplicates:
			CRASH_COND_MSG(p_nodes_to_add.find(r_sync_group_nodes[i].od) != -1, "The function `replace_nodes` must receive unique nodes on each array. Make sure not to add duplicates.");
#endif
		}
	}

	// Add the missing nodes now.
	for (int i = 0; i < int(p_nodes_to_add.size()); i++) {
		NS::ObjectData *od = p_nodes_to_add[i].od;

#ifdef DEBUG_ENABLED
		CRASH_COND_MSG(r_sync_group_nodes.find(od) != -1, "[FATAL] This is impossible to trigger, because the above loop cleaned this.");
#endif

		const uint32_t index = p_sync_group.add_new_sync_object(od, p_is_simulated);
		r_sync_group_nodes[index].update_from(p_nodes_to_add[i]);
	}
}

void NS::SyncGroup::replace_objects(LocalVector<SimulatedObjectInfo> &&p_new_simulated_objects, LocalVector<TrickledObjectInfo> &&p_new_trickled_nodes) {
	replace_nodes_impl(
			*this,
			std::move(p_new_simulated_objects),
			true,
			simulated_sync_objects);

	replace_nodes_impl(
			*this,
			std::move(p_new_trickled_nodes),
			false,
			trickled_sync_objects);
}

void NS::SyncGroup::remove_all_nodes() {
	if (!simulated_sync_objects.is_empty()) {
		simulated_sync_objects.clear();
		simulated_sync_objects_list_changed = true;
	}

	if (!trickled_sync_objects.is_empty()) {
		trickled_sync_objects.clear();
		trickled_sync_objects_list_changed = true;
	}
}

void NS::SyncGroup::notify_new_variable(ObjectData *p_object_data, const std::string &p_var_name) {
	int index = simulated_sync_objects.find(p_object_data);
	if (index >= 0) {
		simulated_sync_objects[index].change.vars.insert(p_var_name);
		simulated_sync_objects[index].change.uknown_vars.insert(p_var_name);
	}
}

void NS::SyncGroup::notify_variable_changed(ObjectData *p_object_data, const std::string &p_var_name) {
	int index = simulated_sync_objects.find(p_object_data);
	if (index >= 0) {
		simulated_sync_objects[index].change.vars.insert(p_var_name);
	}
}

void NS::SyncGroup::set_trickled_update_rate(NS::ObjectData *p_object_data, float p_update_rate) {
	const int index = trickled_sync_objects.find(p_object_data);
	ENSURE(index >= 0);
	trickled_sync_objects[index].update_rate = p_update_rate;
}

float NS::SyncGroup::get_trickled_update_rate(const NS::ObjectData *p_object_data) const {
	for (int i = 0; i < int(trickled_sync_objects.size()); ++i) {
		if (trickled_sync_objects[i].od == p_object_data) {
			return trickled_sync_objects[i].update_rate;
		}
	}
	SceneSynchronizerDebugger::singleton()->print(ERROR, "NodeData " + p_object_data->object_name + " not found into `trickled_sync_objects`.");
	return 0.0;
}

void NS::SyncGroup::sort_trickled_node_by_update_priority() {
	struct DNIComparator {
		_FORCE_INLINE_ bool operator()(const TrickledObjectInfo &a, const TrickledObjectInfo &b) const {
			return a._update_priority > b._update_priority;
		}
	};

	trickled_sync_objects.sort_custom<DNIComparator>();
}

void NS::SyncGroup::notify_peer_has_newly_calculated_latency(int p_peer) {
	if (NS::VecFunc::has(networked_peers, p_peer)) {
		NS::VecFunc::insert_unique(peers_with_newly_calculated_latency, p_peer);
	}
}

void NS::SyncGroup::notify_controller_about_simulating_peers(int p_peer_controller, bool p_simulating) {
	NS::NetworkedControllerBase *controller = scene_sync->get_controller_for_peer(p_peer_controller);
	if (controller) {
		// This is a controller, notify about simulating peers.
		for (int peer : listening_peers) {
			controller->server_set_peer_simulating_this_controller(peer, p_simulating);
		}
	}
}

void NS::SyncGroup::notify_controllers_about_simulating_peer(int p_peer, bool p_simulating) {
	for (const int listening_peer : listening_peers) {
		NetworkedControllerBase *controller = scene_sync->get_controller_for_peer(listening_peer, false);
		controller->server_set_peer_simulating_this_controller(p_peer, p_simulating);
	}
}

std::size_t NS::SyncGroup::find_simulated(const struct ObjectData &p_object_data) const {
	std::size_t i = 0;
	for (auto sso : simulated_sync_objects) {
		if (sso.od == &p_object_data) {
			return i;
		}
		i++;
	}
	return std::numeric_limits<std::size_t>::max();
}

std::size_t NS::SyncGroup::find_trickled(const struct ObjectData &p_object_data) const {
	std::size_t i = 0;
	for (auto toi : trickled_sync_objects) {
		if (toi.od == &p_object_data) {
			return i;
		}
		i++;
	}
	return std::numeric_limits<std::size_t>::max();
}