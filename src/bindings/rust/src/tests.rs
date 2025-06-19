// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Raw FFI bindings to the NIXL library
//!
//! This crate provides low-level bindings to the NIXL C++ library.
//! It is not meant to be used directly, but rather through the higher-level
//! `nixl` crate.

#[cfg(test)]
mod unit_tests {
    use crate::*;
    use std::env;
    use std::time::Duration;

    // Helper function to create an agent with error handling
    fn create_test_agent(name: &str) -> Result<Agent, NixlError> {
        Agent::new(name)
    }

    // Helper function to find a plugin by name
    fn find_plugin(plugins: &StringList, name: &str) -> Result<String, NixlError> {
        plugins
            .iter()
            .filter_map(Result::ok)
            .find(|&plugin| plugin == name)
            .map(ToString::to_string)
            .or_else(|| plugins.get(0).ok().map(ToString::to_string))
            .ok_or(NixlError::InvalidParam)
    }

    #[test]
    fn test_agent_creation() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        drop(agent);
    }

    #[test]
    fn test_agent_invalid_name() {
        let result = Agent::new("test\0agent");
        assert!(matches!(result, Err(NixlError::StringConversionError(_))));
    }

    #[test]
    fn test_get_available_plugins() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let plugins = agent
            .get_available_plugins()
            .expect("Failed to get plugins");

        // Print available plugins
        for plugin in plugins.iter().flatten() {
            println!("Found plugin: {}", plugin);
        }
    }

    #[test]
    fn test_get_plugin_params() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let (_mems, _params) = agent
            .get_plugin_params("UCX")
            .expect("Failed to get plugin params");
        // MemList and Params will be automatically dropped here
    }

    #[test]
    fn test_backend_creation() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let (_mems, params) = agent
            .get_plugin_params("UCX")
            .expect("Failed to get plugin params");
        let backend = agent
            .create_backend("UCX", &params)
            .expect("Failed to create backend");

        let mut opt_args = OptArgs::new().expect("Failed to create opt args");
        opt_args
            .add_backend(&backend)
            .expect("Failed to add backend");
    }

    #[test]
    fn test_params_iteration() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let (mems, params) = agent
            .get_plugin_params("UCX")
            .expect("Failed to get plugin params");

        println!("Parameters:");
        if !params.is_empty().unwrap() {
            for param in params.iter().unwrap() {
                let param = param.unwrap();
                println!("  {} = {}", param.key, param.value);
            }
        } else {
            println!("  (empty)");
        }

        println!("Memory types:");
        if !mems.is_empty().unwrap() {
            for mem_type in mems.iter() {
                println!("  {}", mem_type.unwrap());
            }
        } else {
            println!("  (empty)");
        }
    }

    #[test]
    fn test_get_backend_params() -> Result<(), NixlError> {
        let agent = create_test_agent("test_agent")?;
        let plugins = agent.get_available_plugins()?;

        // Ensure we have at least one plugin
        assert!(!plugins.is_empty()?);

        // Try UCX plugin first since it doesn't require GPU
        let plugin_name = find_plugin(&plugins, "UCX")?;
        let (_mems, params) = agent.get_plugin_params(&plugin_name)?;
        let backend = agent.create_backend(&plugin_name, &params)?;

        // Get backend params after initialization
        let (backend_mems, backend_params) = agent.get_backend_params(&backend)?;

        // Print parameters using iterator
        let param_iter = backend_params.iter()?;
        for param in param_iter.flatten() {
            println!("Backend param: {} = {}", param.key, param.value);
        }

        // Print memory types
        for mem_type in backend_mems.iter().flatten() {
            println!("Backend memory type: {:?}", mem_type);
        }

        Ok(())
    }

    #[test]
    fn test_xfer_dlist() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();

        // Add some descriptors
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.add_desc(0x2000, 0x200, 1).unwrap();

        // Check length
        assert_eq!(dlist.desc_count().unwrap(), 2);

        // Check overlaps
        assert!(!dlist.has_overlaps().unwrap());

        // Add overlapping descriptor
        dlist.add_desc(0x1050, 0x100, 0).unwrap();
        assert!(dlist.has_overlaps().unwrap());

        // Clear list
        dlist.clear().unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 0);

        // Resize list
        dlist.resize(5).unwrap();

        // add descriptors with overlaps
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.add_desc(0x1050, 0x100, 0).unwrap();
        assert!(dlist.has_overlaps().unwrap());
    }

    #[test]
    fn test_reg_dlist() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();

        // Add some descriptors
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.add_desc(0x2000, 0x200, 1).unwrap();

        // Check length
        assert_eq!(dlist.desc_count().unwrap(), 2);

        // Check overlaps
        assert!(!dlist.has_overlaps().unwrap());

        // Add overlapping descriptor
        dlist.add_desc(0x1050, 0x100, 0).unwrap();
        assert!(dlist.has_overlaps().unwrap());

        // Clear list
        dlist.clear().unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 0);

        // Resize list
        dlist.resize(5).unwrap();
    }

    #[test]
    fn test_storage_descriptor_lifetime() {
        // Create storage that outlives the descriptor list
        let storage = SystemStorage::new(1024).unwrap();

        {
            // Create a descriptor list with shorter lifetime
            let mut dlist = XferDescList::new(MemType::Dram).unwrap();
            dlist.add_storage_desc(&storage).unwrap();
            assert_eq!(dlist.desc_count().unwrap(), 1);
            // dlist is dropped here, but storage is still valid
        }

        // MemoryRegion is still valid here
        assert_eq!(<SystemStorage as MemoryRegion>::size(&storage), 1024);
    }

    #[test]
    fn test_multiple_storage_descriptors() {
        let storage1 = SystemStorage::new(1024).unwrap();
        let storage2 = SystemStorage::new(2048).unwrap();

        let mut dlist = XferDescList::new(MemType::Dram).unwrap();

        // Add multiple descriptors
        dlist.add_storage_desc(&storage1).unwrap();
        dlist.add_storage_desc(&storage2).unwrap();

        assert_eq!(dlist.desc_count().unwrap(), 2);
    }

    #[test]
    fn test_memory_registration() {
        let agent = Agent::new("test_agent").unwrap();
        let mut storage = SystemStorage::new(1024).unwrap();
        // Register memory
        storage.register(&agent, None).unwrap();

        // Verify we can still access the memory
        storage.memset(0xAA);
        assert!(storage.as_slice().iter().all(|&x| x == 0xAA));
    }

    #[test]
    fn test_registration_handle_drop() {
        let agent = Agent::new("test_agent").unwrap();
        let mut storage = SystemStorage::new(1024).unwrap();

        // Register memory
        storage.register(&agent, None).unwrap();

        // Drop the storage, which should trigger deregistration
        drop(storage);

        // Create new storage to verify we can register again
        let mut new_storage = SystemStorage::new(1024).unwrap();
        new_storage.register(&agent, None).unwrap();
    }

    #[test]
    fn test_multiple_registrations() {
        let agent = Agent::new("test_agent").unwrap();
        let mut storage1 = SystemStorage::new(1024).unwrap();
        let mut storage2 = SystemStorage::new(2048).unwrap();

        // Register both storages
        storage1.register(&agent, None).unwrap();
        storage2.register(&agent, None).unwrap();

        // Verify we can still access both memories
        storage1.memset(0xAA);
        storage2.memset(0xBB);
        assert!(storage1.as_slice().iter().all(|&x| x == 0xAA));
        assert!(storage2.as_slice().iter().all(|&x| x == 0xBB));
    }

    #[test]
    fn test_make_connection_success() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        // This should succeed if the agent is valid and the backend is set up
        let result = agent.make_connection("remote_agent");
        // Accept either Ok or a backend error if no real remote exists
        assert!(
            result.is_ok() || matches!(result, Err(NixlError::BackendError)),
            "Expected Ok or BackendError, got: {:?}",
            result
        );
    }

    #[test]
    fn test_make_connection_invalid_param() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        // Null bytes in the name should trigger InvalidParam or StringConversionError
        let result = agent.make_connection("remote\0agent");
        assert!(
            matches!(result, Err(NixlError::StringConversionError(_))) ||
            matches!(result, Err(NixlError::InvalidParam)),
            "Expected StringConversionError or InvalidParam, got: {:?}",
            result
        );
    }

    #[test]
    fn test_prep_xfer_dlist_success() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let descs = XferDescList::new(MemType::Dram).unwrap();
        let opt_args = OptArgs::new().unwrap();
        let mut handle = XferDescListHandle::new().unwrap();
        let result = agent.prep_xfer_dlist("remote_agent", &descs, &mut handle, &opt_args);
        // Accept Ok or BackendError if no real remote exists
        assert!(
            result.is_ok() || matches!(result, Err(NixlError::BackendError)),
            "Expected Ok or BackendError, got: {:?}",
            result
        );
    }

    #[test]
    fn test_prep_xfer_dlist_invalid_param() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let descs = XferDescList::new(MemType::Dram).unwrap();
        let opt_args = OptArgs::new().unwrap();
        let mut handle = XferDescListHandle::new().unwrap();
        // Null byte in agent name should trigger error
        let result = agent.prep_xfer_dlist("remote\0agent", &descs, &mut handle, &opt_args);
        assert!(
            matches!(result, Err(NixlError::StringConversionError(_))) ||
            matches!(result, Err(NixlError::InvalidParam)),
            "Expected StringConversionError or InvalidParam, got: {:?}",
            result
        );
    }

    #[test]
    fn test_make_xfer_req_success() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let local_descs = XferDescList::new(MemType::Dram).unwrap();
        let remote_descs = XferDescList::new(MemType::Dram).unwrap();
        let opt_args = OptArgs::new().unwrap();
        let result = agent.make_xfer_req(
            XferOp::Read,
            &local_descs,
            &remote_descs,
            "remote_agent",
            &opt_args,
        );
        // Accept Ok or BackendError if no real remote exists
        assert!(
            result.is_ok() || matches!(result, Err(NixlError::BackendError)),
            "Expected Ok or BackendError, got: {:?}",
            result.err()
        );
    }

    #[test]
    fn test_make_xfer_req_invalid_param() {
        let agent = Agent::new("test_agent").expect("Failed to create agent");
        let local_descs = XferDescList::new(MemType::Dram).unwrap();
        let remote_descs = XferDescList::new(MemType::Dram).unwrap();
        let opt_args = OptArgs::new().unwrap();
        // Null byte in remote_agent should trigger error
        let result = agent.make_xfer_req(
            XferOp::Read,
            &local_descs,
            &remote_descs,
            "remote\0agent",
            &opt_args,
        );
        assert!(
            matches!(result, Err(NixlError::StringConversionError(_))) ||
            matches!(result, Err(NixlError::InvalidParam)),
            "Expected StringConversionError or InvalidParam, got: {}",
            result.err().unwrap()
        );
    }

    #[test]
    fn test_get_local_md() {
        let agent = Agent::new("test_agent").unwrap();

        // Get available plugins and print their names
        let plugins = agent.get_available_plugins().unwrap();
        for plugin in plugins.iter().flatten() {
            println!("Found plugin: {}", plugin);
        }

        // Get plugin parameters for both agents
        let (_mem_list, params) = agent.get_plugin_params("UCX").unwrap();

        // Create backends for both agents
        let backend1 = agent.create_backend("UCX", &params).unwrap();

        let md = agent.get_local_md().unwrap();

        // Measure the size
        let initial_size = md.len();
        println!("Local metadata size: {}", initial_size);

        let mut opt_args = OptArgs::new().unwrap();
        opt_args.add_backend(&backend1).unwrap();

        let mut storages = Vec::new();

        for _i in 0..10 {
            // Register some memory regions
            let mut storage = SystemStorage::new(1024).unwrap();
            storage.register(&agent, Some(&opt_args)).unwrap();
            storages.push(storage);
        }

        let md = agent.get_local_md().unwrap();

        // Measure the size again
        let final_size = md.len();
        println!("Local metadata size: {}", final_size);

        // Check if the size has increased
        assert!(final_size > initial_size);
    }

    #[test]
    fn test_metadata_exchange() {
        // Create two agents
        let agent2 = Agent::new("agent2").unwrap();
        let agent1 = Agent::new("agent1").unwrap();

        // Get plugin parameters for both agents
        let (_mem_list, params) = agent1.get_plugin_params("UCX").unwrap();

        // Create backends for both agents
        let _backend1 = agent1.create_backend("UCX", &params).unwrap();
        let _backend2 = agent2.create_backend("UCX", &params).unwrap();

        // Get metadata from agent1
        let md = agent1.get_local_md().unwrap();

        // Load metadata into agent2
        let remote_name = agent2.load_remote_md(&md).unwrap();
        assert_eq!(remote_name, "agent1");
    }

    #[test]
    fn test_basic_agent_lifecycle() -> Result<(), NixlError> {
        // Create agents
        let agent2 = create_test_agent("A2")?;
        let agent1 = create_test_agent("A1")?;

        // Print available plugins
        let plugins = agent1.get_available_plugins()?;
        for plugin in plugins.iter().flatten() {
            println!("Found plugin: {}", plugin);
        }

        // Setup UCX backends
        let (_mem_list1, _params) = agent1.get_plugin_params("UCX")?;
        let (_mem_list2, params) = agent2.get_plugin_params("UCX")?;

        let _backend1 = agent1.create_backend("UCX", &params)?;
        let _backend2 = agent2.create_backend("UCX", &params)?;

        // Setup memory regions
        let mut storage1 = SystemStorage::new(256)?;
        let mut storage2 = SystemStorage::new(256)?;

        // Initialize memory patterns
        storage1.memset(0xbb);
        storage2.memset(0x00);

        // Verify initial memory patterns
        assert!(storage1.as_slice().iter().all(|&x| x == 0xbb));
        assert!(storage2.as_slice().iter().all(|&x| x == 0x00));

        // Create registration descriptor lists
        storage1.register(&agent1, None).unwrap();
        storage2.register(&agent2, None).unwrap();

        // Exchange metadata
        let metadata = agent2.get_local_md()?;
        let remote_name = agent1.load_remote_md(&metadata)?;
        assert_eq!(remote_name, "A2");

        // Setup transfer descriptors
        let mut local_xfer_dlist = XferDescList::new(MemType::Dram)?;
        let mut remote_xfer_dlist = XferDescList::new(MemType::Dram)?;
        local_xfer_dlist.add_storage_desc(&storage1)?;
        remote_xfer_dlist.add_storage_desc(&storage2)?;

        // Setup transfer arguments
        let mut xfer_args = OptArgs::new()?;
        xfer_args.set_has_notification(true)?;
        xfer_args.set_notification_message(b"notification")?;

        // Create and post transfer request
        let xfer_req = agent1.create_xfer_req(
            XferOp::Write,
            &local_xfer_dlist,
            &remote_xfer_dlist,
            &remote_name,
            Some(&xfer_args),
        )?;

        // Handle transfer request
        if let Ok(status) = agent1.post_xfer_req(&xfer_req, None) {
            println!("Transfer request posted with status: {}", status);

            if status {
                // Wait for transfer completion with timeout
                let timeout = Duration::from_secs(5);
                let start = std::time::Instant::now();

                while start.elapsed() < timeout {
                    match agent1.get_xfer_status(&xfer_req) {
                        Ok(false) => {
                            println!("Transfer completed");
                            break;
                        }
                        Ok(true) => std::thread::sleep(Duration::from_millis(100)),
                        Err(e) => {
                            println!("Error getting transfer status: {:?}", e);
                            break;
                        }
                    }
                }

                // Wait for notifications with timeout
                let mut notifs = NotificationMap::new()?;
                let start = std::time::Instant::now();

                while start.elapsed() < timeout {
                    match agent2.get_notifications(&mut notifs, None) {
                        Ok(_) if !notifs.is_empty()? => {
                            println!("Got notifications");
                            break;
                        }
                        Ok(_) => std::thread::sleep(Duration::from_millis(100)),
                        Err(e) => {
                            println!("Error getting notifications: {:?}", e);
                            break;
                        }
                    }
                }

                // Verify notification if received
                if !notifs.is_empty()? {
                    let mut agents = notifs.agents();
                    if let Some(Ok(agent_name)) = agents.next() {
                        let mut notifications = notifs.get_notifications(agent_name)?;
                        if let Some(Ok(notif)) = notifications.next() {
                            assert_eq!(notif, b"notification");

                            // Verify transfer if completed
                            if !agent1.get_xfer_status(&xfer_req)? {
                                assert!(storage2.as_slice().iter().all(|&x| x == 0xbb));
                            }
                        }
                    }
                }
            }
        }

        // Verify source memory remains unchanged
        assert!(storage1.as_slice().iter().all(|&x| x == 0xbb));

        Ok(())
    }

    #[test]
    fn test_etcd_metadata_exchange() -> Result<(), NixlError> {
        // Check if NIXL_ETCD_ENDPOINTS env var is set to skip test if not
        if env::var("NIXL_ETCD_ENDPOINTS").is_err() {
            println!("Skipping etcd test - NIXL_ETCD_ENDPOINTS not set");
            return Ok(());
        }

        // Create two agents for metadata exchange
        let agent1 = Agent::new("EtcdAgent1")?;
        let agent2 = Agent::new("EtcdAgent2")?;

        // Get UCX backend to add to optional arguments
        let plugins = agent1.get_available_plugins()?;
        let plugin_name = find_plugin(&plugins, "UCX")?;
        let (_mems, params) = agent1.get_plugin_params(&plugin_name)?;
        let backend = agent1.create_backend(&plugin_name, &params)?;

        // Create OptArgs with backend
        let mut opt_args = OptArgs::new()?;
        opt_args.add_backend(&backend)?;

        // Send agent1's metadata to etcd
        agent1.send_local_md(Some(&opt_args))?;
        println!("Successfully sent agent1 metadata to etcd");

        // Fetch agent1's metadata from etcd with agent2
        agent2.fetch_remote_md("EtcdAgent1", Some(&opt_args))?;
        println!("Successfully fetched agent1 metadata from etcd");

        // Invalidate agent1's metadata in etcd
        agent1.invalidate_local_md(Some(&opt_args))?;
        println!("Successfully invalidated agent1 metadata in etcd");

        Ok(())
    }

    #[test]
    fn test_send_notification() -> Result<(), NixlError> {
        // Create two agents for notification exchange
        let agent1 = Agent::new("NotifSender")?;
        let agent2 = Agent::new("NotifReceiver")?;

        // Set up backends for both agents
        let (_mem_list, params) = agent1.get_plugin_params("UCX")?;
        let backend1 = agent1.create_backend("UCX", &params)?;
        let backend2 = agent2.create_backend("UCX", &params)?;

        // Exchange metadata
        let metadata = agent2.get_local_md()?;
        agent1.load_remote_md(&metadata)?;

        // Create notification message
        let message = b"Test notification message";

        // Send notification with no backend specified
        agent1.send_notification("NotifReceiver", message, None)?;

        // Send notification with specific backend
        agent1.send_notification("NotifReceiver", message, Some(&backend1))?;

        // Create a notification map to receive notifications
        let mut notifs = NotificationMap::new()?;

        // Receive notifications without backend
        agent2.get_notifications(&mut notifs, None)?;

        // Receive notifications with specific backend
        let mut opt_args = OptArgs::new()?;
        opt_args.add_backend(&backend2)?;
        agent2.get_notifications(&mut notifs, Some(&opt_args))?;

        // Verify notification map contents
        if !notifs.is_empty()? {
            let mut agents = notifs.agents();

            // Should have notifications from NotifSender
            if let Some(Ok(agent_name)) = agents.next() {
                assert_eq!(agent_name, "NotifSender");

                // Verify notification content
                let notifications = notifs.get_notifications(agent_name)?;
                let notif_count = notifs.get_notifications_size(agent_name)?;

                // May have 1 or 2 notifications depending on whether both were processed
                assert!(notif_count > 0, "Should have at least one notification");

                // Check content of notification
                for notification in notifications {
                    assert_eq!(notification?, message);
                }
            }
        }

        Ok(())
    }

    #[test]
    fn test_check_remote_metadata() {
        // Create two agents
        let agent1 = Agent::new("agent1").expect("Failed to create agent1");
        let agent2 = Agent::new("agent2").expect("Failed to create agent2");

        // Set up backends for both agents (required before metadata operations)
        let (_mem_list, params) = agent1
            .get_plugin_params("UCX")
            .expect("Failed to get plugin params");
        let _backend1 = agent1
            .create_backend("UCX", &params)
            .expect("Failed to create backend for agent1");
        let _backend2 = agent2
            .create_backend("UCX", &params)
            .expect("Failed to create backend for agent2");

        // Initially, agent1 should not have metadata for agent2
        assert!(!agent1.check_remote_metadata("agent2", None));

        // Get and share metadata
        let metadata = agent2.get_local_md().expect("Failed to get local metadata");
        agent1
            .load_remote_md(&metadata)
            .expect("Failed to load remote metadata");

        // Now agent1 should have metadata for agent2
        assert!(agent1.check_remote_metadata("agent2", None));

        // Test with a descriptor list
        let mut storage = SystemStorage::new(1024).expect("Failed to create storage");
        let opt_args = OptArgs::new().expect("Failed to create opt args");
        storage
            .register(&agent2, Some(&opt_args))
            .expect("Failed to register memory");

        // Create descriptor list with memory that exists in agent2
        let mem_type = MemType::Dram;
        let mut xfer_desc_list =
            XferDescList::new(mem_type).expect("Failed to create xfer desc list");
        xfer_desc_list
            .add_desc(
                unsafe { storage.as_ptr() } as usize,
                storage.size(),
                storage.device_id(),
            )
            .expect("Failed to add descriptor");

        // Update metadata after registration
        let metadata = agent2
            .get_local_md()
            .expect("Failed to get updated local metadata");
        agent1
            .load_remote_md(&metadata)
            .expect("Failed to reload remote metadata");

        // Check with descriptor list - should return true for valid descriptors
        assert!(agent1.check_remote_metadata("agent2", Some(&xfer_desc_list)));

        // Create a descriptor list with invalid memory address
        let mut invalid_desc_list =
            XferDescList::new(mem_type).expect("Failed to create invalid desc list");
        invalid_desc_list
            .add_desc(0xdeadbeef, 1024, 0)
            .expect("Failed to add invalid descriptor");

        // Check with invalid descriptor list - should return false
        assert!(!agent1.check_remote_metadata("agent2", Some(&invalid_desc_list)));

        // Check with non-existent agent name
        assert!(!agent1.check_remote_metadata("non_existent_agent", None));

        // Check with invalid agent name (contains null byte)
        // The function should return false rather than panic
        let invalid_name = "invalid\0agent";
        assert!(!agent1.check_remote_metadata(invalid_name, None));
    }

    #[test]
    fn test_xfer_desc_list_new_and_new_sorted() {
        let dlist = XferDescList::new(MemType::Dram).unwrap();
        assert!(dlist.is_empty().unwrap());
        let dlist_sorted = XferDescList::new_sorted(MemType::Dram).unwrap();
        assert!(dlist_sorted.is_empty().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_new_sorted_sortedness() {
        let dlist = XferDescList::new_sorted(MemType::Dram).unwrap();
        let sorted = dlist.is_sorted().unwrap();
        assert!(sorted);
        let dlist_unsorted = XferDescList::new(MemType::Dram).unwrap();
        let unsorted = dlist_unsorted.is_sorted().unwrap();
        assert!(!unsorted);
    }

    #[test]
    fn test_xfer_desc_list_get_type() {
        let dlist = XferDescList::new(MemType::Vram).unwrap();
        assert_eq!(dlist.get_type().unwrap(), MemType::Vram);
    }

    #[test]
    fn test_xfer_desc_list_get_type_after_add() {
        let mut dlist = XferDescList::new(MemType::Block).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert_eq!(dlist.get_type().unwrap(), MemType::Block);
    }

    #[test]
    fn test_xfer_desc_list_verify_sorted_true() {
        let mut dlist = XferDescList::new_sorted(MemType::Dram).unwrap();

        // list size should be at least 1 to be considered sorted
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.verify_sorted().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_verify_sorted_false() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x2000, 0x100, 0).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap(); // out of order
        assert!(!dlist.verify_sorted().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_desc_count_basic() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 0);
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 1);
    }

    #[test]
    fn test_xfer_desc_list_desc_count_after_clear() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.clear().unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 0);
    }

    #[test]
    fn test_xfer_desc_list_is_empty_true() {
        let dlist = XferDescList::new(MemType::Dram).unwrap();
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_is_empty_false() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(!dlist.is_empty().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_is_sorted_true() {
        let mut dlist = XferDescList::new_sorted(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.is_sorted().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_is_sorted_false() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x2000, 0x100, 0).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(!dlist.is_sorted().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_trim_basic() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.trim().unwrap();
        assert!(dlist.desc_count().unwrap() <= 1);
    }

    #[test]
    fn test_xfer_desc_list_trim_empty() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        assert!(dlist.trim().is_ok());
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_rem_desc_basic() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.rem_desc(0).is_ok());
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_rem_desc_out_of_bounds() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        assert!(dlist.rem_desc(0).is_err());
    }

    #[test]
    fn test_xfer_desc_list_clear_basic() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.clear().unwrap();
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_clear_empty() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        assert!(dlist.clear().is_ok());
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_xfer_desc_list_print_basic() {
        let dlist = XferDescList::new(MemType::Dram).unwrap();
        assert!(dlist.print().is_ok());
    }

    #[test]
    fn test_xfer_desc_list_print_after_add() {
        let mut dlist = XferDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.print().is_ok());
    }

    // ----------- RegDescList API TESTS -----------

    #[test]
    fn test_reg_desc_list_new_and_new_sorted() {
        let dlist = RegDescList::new(MemType::Dram).unwrap();
        assert!(dlist.is_empty().unwrap());
        let dlist_sorted = RegDescList::new_sorted(MemType::Dram).unwrap();
        assert!(dlist_sorted.is_empty().unwrap());
    }

    #[test]
    fn test_reg_desc_list_new_sorted_sortedness() {
        let dlist = RegDescList::new_sorted(MemType::Dram).unwrap();
        assert!(dlist.is_sorted().unwrap());
        let dlist_unsorted = RegDescList::new(MemType::Dram).unwrap();
        assert!(!dlist_unsorted.is_sorted().unwrap());
    }

    #[test]
    fn test_reg_desc_list_get_type() {
        let dlist = RegDescList::new(MemType::Vram).unwrap();
        assert_eq!(dlist.get_type().unwrap(), MemType::Vram);
    }

    #[test]
    fn test_reg_desc_list_get_type_after_add() {
        let mut dlist = RegDescList::new(MemType::Block).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert_eq!(dlist.get_type().unwrap(), MemType::Block);
    }

    #[test]
    fn test_reg_desc_list_verify_sorted_true() {
        let mut dlist = RegDescList::new_sorted(MemType::Dram).unwrap();

        // list size should be at least 1 to be considered sorted
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.verify_sorted().unwrap());
    }

    #[test]
    fn test_reg_desc_list_verify_sorted_false() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x2000, 0x100, 0).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap(); // out of order
        assert!(!dlist.verify_sorted().unwrap());
    }

    #[test]
    fn test_reg_desc_list_desc_count_basic() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 0);
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 1);
    }

    #[test]
    fn test_reg_desc_list_desc_count_after_clear() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.clear().unwrap();
        assert_eq!(dlist.desc_count().unwrap(), 0);
    }

    #[test]
    fn test_reg_desc_list_is_empty_true() {
        let dlist = RegDescList::new(MemType::Dram).unwrap();
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_reg_desc_list_is_empty_false() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(!dlist.is_empty().unwrap());
    }

    #[test]
    fn test_reg_desc_list_is_sorted_true() {
        let mut dlist = RegDescList::new_sorted(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.is_sorted().unwrap());
    }

    #[test]
    fn test_reg_desc_list_is_sorted_false() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x2000, 0x100, 0).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(!dlist.is_sorted().unwrap());
    }

    #[test]
    fn test_reg_desc_list_trim_basic() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.trim().unwrap();
        assert!(dlist.desc_count().unwrap() <= 1);
    }

    #[test]
    fn test_reg_desc_list_trim_empty() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        assert!(dlist.trim().is_ok());
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_reg_desc_list_rem_desc_basic() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.rem_desc(0).is_ok());
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_reg_desc_list_rem_desc_out_of_bounds() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        assert!(dlist.rem_desc(0).is_err());
    }

    #[test]
    fn test_reg_desc_list_clear_basic() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        dlist.clear().unwrap();
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_reg_desc_list_clear_empty() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        assert!(dlist.clear().is_ok());
        assert!(dlist.is_empty().unwrap());
    }

    #[test]
    fn test_reg_desc_list_print_basic() {
        let dlist = RegDescList::new(MemType::Dram).unwrap();
        assert!(dlist.print().is_ok());
    }

    #[test]
    fn test_reg_desc_list_print_after_add() {
        let mut dlist = RegDescList::new(MemType::Dram).unwrap();
        dlist.add_desc(0x1000, 0x100, 0).unwrap();
        assert!(dlist.print().is_ok());
    }
}
