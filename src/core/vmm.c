/**
 * CROSSCONHyp, a Lightweight Static Partitioning Hypervisor
 *
 * Copyright (c) bao Project (www.bao-project.org), 2019-
 *
 * Authors:
 *      Jose Martins <jose.martins@bao-project.org>
 *      Sandro Pinto <sandro.pinto@bao-project.org>
 *
 * CROSSCONHyp is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2 as published by the Free
 * Software Foundation, with a special exception exempting guest code from such
 * license. See the COPYING file in the top-level directory for details.
 *
 */

#include <vmm.h>
#include <vm.h>
#include <config.h>
#include <cpu.h>
#include <iommu.h>
#include <spinlock.h>
#include <fences.h>
#include <string.h>
#include <ipc.h>
#include <vmstack.h>
#include "list.h"
#include "objcache.h"
#include "util.h"

struct config* vm_config_ptr;

struct partition* const partition = (struct partition*)CROSSCONHYP_VM_BASE;

static void* vmm_alloc_vm_struct()
{
    size_t vm_npages = NUM_PAGES(sizeof(struct vm));
    vaddr_t va = mem_alloc_vpage(&cpu.as, SEC_HYP_VM, (vaddr_t)NULL, vm_npages);
    mem_map(&cpu.as, va, NULL, vm_npages, PTE_HYP_FLAGS);
    memset((void*)va, 0, vm_npages * PAGE_SIZE);
    return (void*)va;
}

static void vmm_free_vm_struct(struct vm* vm)
{
    size_t n = NUM_PAGES(sizeof(struct vm));
    memset((void*)vm, 0, n * PAGE_SIZE);
    mem_free_vpage(&cpu.as, (vaddr_t)vm, n, true);
}

uint64_t vmm_alloc_vmid()
{
    static uint64_t id = 0;
    static spinlock_t lock = SPINLOCK_INITVAL;

    uint64_t vmid;
    spin_lock(&lock);
    vmid = ++id;  // no vmid 0
    spin_unlock(&lock);

    return vmid;
}

static struct vcpu* vmm_create_vms(struct vm_config* config, struct vcpu* parent)
{
    if (cpu.id == partition->master) {
        partition->init.curr_vm = vmm_alloc_vm_struct();
        partition->init.ncpus = 0;
    }

    if (parent) {
        cpu_sync_barrier(&parent->vm->sync);
    } else {
        cpu_sync_barrier(&partition->sync);
    }

    struct vm* vm = partition->init.curr_vm;
    struct vcpu* vcpu = NULL;

    bool assigned = false;
    bool master = false;

    spin_lock(&partition->lock);
    if ((partition->init.ncpus < config->platform.cpu_num) &&
        (1ULL << cpu.id) & config->cpu_affinity) {
        if (partition->init.ncpus == 0) master = true;
        partition->init.ncpus++;
        assigned = true;
    }
    spin_unlock(&partition->lock);

    if (parent) {
        cpu_sync_barrier(&parent->vm->sync);
    } else {
        cpu_sync_barrier(&partition->sync);
    }

    spin_lock(&partition->lock);
    if (!assigned && (partition->init.ncpus < config->platform.cpu_num)) {
        if (partition->init.ncpus == 0) master = true;
        partition->init.ncpus++;
        assigned = true;
    }
    spin_unlock(&partition->lock);

    if(assigned){
        size_t vm_id = -1;
        if(master)
            vm_id = vmm_alloc_vmid();
        vcpu = vm_init(vm, config, master, vm_id);
        for(int i = 0; i < config->children_num; i++){
            struct vm_config* child_config = config->children[i];
            struct vcpu* child = vmm_create_vms(child_config, vcpu); //TODO: do this without recursion
            if(child != NULL){
                struct node_data* node = objcache_alloc(&partition->nodes);
                node->data = child;
                INFO("VM %u is parent of VM %u", vcpu->vm->id, child->vm->id);
                list_push(&vcpu->vmstack_children, (node_t*)node);
            }
            cpu_sync_barrier(&vm->sync);
        }
    }

    return vcpu;
}

struct vm* vmm_init_dynamic(struct config* ptr_vm_config, uint64_t vm_addr)
{
    vmid_t vmid = 0;
    struct vm* vm = vmm_alloc_vm_struct();

    vmid = vmm_alloc_vmid();
    vm_init_dynamic(vm, ptr_vm_config, vm_addr, vmid);

    /* TODO */
    struct node_data* node = objcache_alloc(&partition->nodes);
    /* TODO if more than one CPU is created obtain the vcpu for the current cpu */
    struct vcpu* child = vm_get_vcpu(vm, 0);
    node->data = child;
    list_push(&cpu.vcpu->vmstack_children, (node_t*)node);

    return vm;
}

void vmm_destroy_dynamic(struct vm *vm)
{
    list_foreach(cpu.vcpu->vmstack_children, struct node_data, node){
	struct vcpu* child = node->data;
	if(child->vm == vm){
            /* TODO remove recursively */
	    list_rm(&cpu.vcpu->vmstack_children, (node_t*)node);
	    objcache_free(&partition->nodes, node);
	}
    }

    vm_destroy_dynamic(vm);
    vmm_free_vm_struct(vm);
}

void vmm_init()
{
    if(vm_config_ptr->vmlist_size == 0){
        if(cpu.id == CPU_MASTER)
            INFO("No virtual machines to run.");
        cpu_idle();
    }

    vmm_arch_init();

    static struct vm_assignment {
        spinlock_t lock;
        bool master;
        size_t ncpus;
        cpumap_t cpus;
        pte_t vm_shared_table;
    } * vm_assign;

    size_t vmass_npages = 0;
    if (cpu.id == CPU_MASTER) {
        iommu_init();

        vmass_npages =
            ALIGN(sizeof(struct vm_assignment) * vm_config_ptr->vmlist_size,
                  PAGE_SIZE) /
            PAGE_SIZE;
        vm_assign = mem_alloc_page(vmass_npages, SEC_HYP_GLOBAL, false);
        if (vm_assign == NULL) ERROR("cant allocate vm assignemnt pages");
        memset((void*)vm_assign, 0, vmass_npages * PAGE_SIZE);
    }

    cpu_sync_barrier(&cpu_glb_sync);

    bool master = false;
    bool assigned = false;
    vmid_t vm_id = 0;
    struct vm_config *vm_config = NULL;

    /**
     * Assign cpus according to vm affinity.
     */
    for (size_t i = 0; i < vm_config_ptr->vmlist_size && !assigned; i++) {
        if (vm_config_ptr->vmlist[i]->cpu_affinity & (1UL << cpu.id)) {
            spin_lock(&vm_assign[i].lock);
            if (!vm_assign[i].master) {
                vm_assign[i].master = true;
                vm_assign[i].ncpus++;
                vm_assign[i].cpus |= (1UL << cpu.id);
                master = true;
                assigned = true;
                vm_id = i;
            } else if (vm_assign[i].ncpus <
                       vm_config_ptr->vmlist[i]->platform.cpu_num) {
                assigned = true;
                vm_assign[i].ncpus++;
                vm_assign[i].cpus |= (1UL << cpu.id);
                vm_id = i;
            }
            spin_unlock(&vm_assign[i].lock);
        }
    }

    cpu_sync_barrier(&cpu_glb_sync);

    /**
     * Assign remaining cpus not assigned by affinity.
     */
    if (assigned == false) {
        for (size_t i = 0; i < vm_config_ptr->vmlist_size && !assigned; i++) {
            spin_lock(&vm_assign[i].lock);
            if (vm_assign[i].ncpus <
                vm_config_ptr->vmlist[i]->platform.cpu_num) {
                if (!vm_assign[i].master) {
                    vm_assign[i].master = true;
                    vm_assign[i].ncpus++;
                    master = true;
                    assigned = true;
                    vm_assign[i].cpus |= (1UL << cpu.id);
                    vm_id = i;
                } else {
                    assigned = true;
                    vm_assign[i].ncpus++;
                    vm_assign[i].cpus |= (1UL << cpu.id);
                    vm_id = i;
                }
            }
            spin_unlock(&vm_assign[i].lock);
        }
    }

    cpu_sync_barrier(&cpu_glb_sync);

    if (assigned) {
        vm_config = vm_config_ptr->vmlist[vm_id];
        if (master) {
            //size_t vm_npages = NUM_PAGES(sizeof(struct vm));
            /* TODO */

            /* Alloc partition memory (stack of VMs) */
            size_t vm_npages = NUM_PAGES(sizeof(struct partition));
            vaddr_t va = mem_alloc_vpage(&cpu.as, SEC_HYP_VM, (vaddr_t)CROSSCONHYP_VM_BASE, vm_npages);
            mem_map(&cpu.as, va, NULL, vm_npages, PTE_HYP_FLAGS);
            memset((void*)va, 0, vm_npages * PAGE_SIZE);

            /* Initialize partition */
            cpu_sync_init(&partition->sync, vm_assign[vm_id].ncpus);
            partition->master = cpu.id;
            objcache_init(&partition->nodes, sizeof(struct node_data), SEC_HYP_VM, true);

            fence_ord_write();

            vm_assign[vm_id].vm_shared_table = *pt_get_pte(&cpu.as.pt, 0, (vaddr_t)CROSSCONHYP_VM_BASE);
        } else {
            while (vm_assign[vm_id].vm_shared_table == 0);
            pte_t* pte = pt_get_pte(&cpu.as.pt, 0, (vaddr_t)CROSSCONHYP_VM_BASE);
            *pte = vm_assign[vm_id].vm_shared_table;
            fence_sync_write();
        }
    }

    cpu_sync_barrier(&cpu_glb_sync);

    if (cpu.id == CPU_MASTER) {
        mem_free_vpage(&cpu.as, (vaddr_t)vm_assign, vmass_npages, true);
    }

    ipc_init(vm_config, master);

    struct vcpu* root;
    if (assigned) {
        root = vmm_create_vms(vm_config_ptr->vmlist[vm_id], NULL);
        cpu_sync_barrier(&partition->sync);
        vmstack_push(root);
        vcpu_run(root);
    }

    cpu_idle();
}
