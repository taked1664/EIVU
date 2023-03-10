#pragma once

#if 1
#define PROC_BUF_HEADER(addr) \
            set_id(addr, (get_id(addr) + 1) & 2047); \
            set_len(addr, (get_len(addr) + 1) & 2047);
#else
#define PROC_BUF_HEADER(addr)
#endif

#if BUF_HEADER_SIZE > 0
#define PREFETCH_BUF(addr1, addr2) \
        prefetch0(addr1); \
        prefetch0(addr2);
#else
#define PREFETCH_BUF(addr1, addr2);
#endif

#define PREFETCH_POOL(addr) \
        prefetch0(addr);

static inline void prefetch0(const volatile void *p) {
    asm volatile ("prefetcht0 %[p]" : : [p] "m"(*(const volatile char *) p));
}

static inline void cldemote(const volatile void *p) {
    asm volatile(".byte 0x0f, 0x1c, 0x06"::"S" (p));
}

void set_used_flag(desc *d) {
    volatile int16_t *flags = &d->flags;
    int16_t f;
    __atomic_load(flags, &f, __ATOMIC_ACQUIRE);
    f |= USED_FLAG;
    f &= ~AVAIL_FLAG;
    __atomic_store(flags, &f, __ATOMIC_RELEASE);
}

void set_avail_flag(desc *d) {
    volatile int16_t *flags = &d->flags;
    int16_t f;
    __atomic_load(flags, &f, __ATOMIC_ACQUIRE);
    f &= ~USED_FLAG;
    f |= AVAIL_FLAG;
    __atomic_store(flags, &f, __ATOMIC_RELEASE);
}

static constexpr uint64_t WAIT_COUNT_THRESH = 1000000000;

void wait_used(vq *v, int desc_idx) {
    static uint64_t wait_counter = 0;
    // Wait until packets are available
    while ((*((volatile int16_t *) &v->descs[desc_idx].flags) & USED_FLAG) == 0) {
        if (wait_counter++ >= WAIT_COUNT_THRESH) {
            throw std::runtime_error("wait_used");
        }
    }
    wait_counter = 0;
}

void wait_avail(vq *v, int desc_idx) {
    static uint64_t wait_counter = 0;
    // Wait untile empty packet buffers are provided
    while ((*((volatile int16_t *) &v->descs[desc_idx].flags) & AVAIL_FLAG) == 0) {
        if (wait_counter++ >= WAIT_COUNT_THRESH) {
            throw std::runtime_error("wait_avail");
        }
    }
    wait_counter = 0;
}

void init_ring(vq *v, desc *d) {
    v->size = VQ_ENTRY_NUM;
    v->last_used_idx = 0;
    v->last_avail_idx = 0;
    v->last_inflight_idx = 0;
    v->descs = d;
}

char ids[32] = {21, 10, 24, 22, 15, 31, 0, 30, 14, 1, 11, 2, 13, 23, 12, 3, 25, 17, 4, 16, 26, 19, 5, 28, 20, 6, 27, 7,
                8, 18, 29, 9};

void send_rx_to_guest(vq *vq_rx, buf **buf_src, guest_buffer_pool *pool_guest, int num_fin, bool is_stream) {
    const uint16_t RING_SIZE_MASK = vq_rx->size - 1;

    for (int i = 0; i < num_fin; i++) {
#if defined(READ_HEADER4_RX) || defined(WRITE_HEADER4_RX)
        PREFETCH_BUF(buf_src[i]->header.id_addr, buf_src[i]->header.id_addr);
#else
        PREFETCH_BUF(buf_src[i]->header.id_addr, buf_src[i]->header.len_addr);
#endif
//        int64_t copy_dest_index = vq_rx->descs[vq_rx->last_used_idx + i].entry_index;
//        auto *copy_dest_addr = get_packet_addr(&pool_guest->buffers[copy_dest_index]);
//        PREFETCH_POOL(copy_dest_addr);
    }

    wait_avail(vq_rx, (vq_rx->last_used_idx + num_fin - 1) & RING_SIZE_MASK);

#if BUF_HEADER_SIZE > 0
    for (int i = 0; i < num_fin; i++) {
#ifdef READ_HEADER4_RX
        if (*(int *) (buf_src[i]->header.id_addr) == 999999) {
            exit(1);
        }
#elif defined(WRITE_HEADER4_RX)
        memset(buf_src[i]->header.id_addr, i, 4);
#else
        PROC_BUF_HEADER(buf_src[i]);
#endif
    }
#endif

    int last_used_idx_shadow = vq_rx->last_used_idx;
    for (int i = 0; i < num_fin; i++) {
        if (!is_stream) {
            int64_t copy_dest_index = vq_rx->descs[vq_rx->last_used_idx].entry_index;

            int8_t vio_header[VIO_HEADER_SIZE];
            load_vio_header(&pool_guest->buffers[copy_dest_index], &vio_header);
            vio_header[0] += 1;
            store_vio_header(&pool_guest->buffers[copy_dest_index], &vio_header);

#ifndef ZERO_COPY_RX
            auto *copy_dest_addr = get_packet_addr(&pool_guest->buffers[copy_dest_index]);
#if defined(SIMD_COPY_RX) || defined(NON_TEMPORAL_COPY_RX)
            void *xmm01 = copy_dest_addr;
            auto *xmm02 = get_packet_addr(buf_src[i]);
            for (int j = 0; j < NUM_LOOP; j++) {
#ifdef SIMD_COPY_RX
                _mm256_store_si256((__m256i *) xmm01 + j, _mm256_load_si256((__m256i *) xmm02 + j));
#else
                _mm256_stream_si256((__m256i *) xmm01 + j, _mm256_stream_load_si256((__m256i *) xmm02 + j));
#endif
            }
#else
            memcpy(copy_dest_addr, get_packet_addr(buf_src[i]), SIZE_PACKET);
#endif
            //cldemote(copy_dest_addr);
            //_mm_clflushopt(copy_dest_addr);
#endif
        }

        // Update index
        vq_rx->last_used_idx++;
        vq_rx->last_used_idx &= RING_SIZE_MASK;
    }

#ifdef SKIP_INDEX_RX
    int skipped_index = 64 / VQ_ENTRY_SIZE;
    for (int i = skipped_index; i < num_fin; i++) {
#else
    for (int i = 0; i < num_fin; i++) {
#endif
        int desc_index = (last_used_idx_shadow + i) & RING_SIZE_MASK;
        set_used_flag(&vq_rx->descs[desc_index]);
    }

#ifdef SKIP_INDEX_RX
    for (int i = 0; i < skipped_index; i++) {
        int desc_index = (last_used_idx_shadow + i) % VQ_ENTRY_NUM;
        set_used_flag(&vq_rx->descs[desc_index]);
    }
#endif
}

void send_guest_to_tx(vq *vq_tx, buf **buf_dest, guest_buffer_pool *pool_guest, int num_fin, bool is_stream) {
    const uint16_t RING_SIZE_MASK = vq_tx->size - 1;

    wait_used(vq_tx, (vq_tx->last_avail_idx + num_fin - 1) & RING_SIZE_MASK);

    for (int i = 0; i < num_fin; i++) {
#if defined(READ_HEADER4_Tx) || defined(WRITE_HEADER4_Tx)
        PREFETCH_BUF(buf_dest[i]->header.id_addr, buf_dest[i]->header.id_addr);
#else
        PREFETCH_BUF(buf_dest[i]->header.id_addr, buf_dest[i]->header.len_addr);
#endif
//        int entry_idx = vq_tx->descs[vq_tx->last_avail_idx].entry_index;
//        auto copy_src_addr = get_packet_addr(&pool_guest->buffers[entry_idx]);
//        PREFETCH_POOL(copy_src_addr);
    }

    int last_avail_idx_shadow = vq_tx->last_avail_idx;
    for (int i = 0; i < num_fin; i++) {
        if (!is_stream) {
            int entry_idx = vq_tx->descs[vq_tx->last_avail_idx].entry_index;

            int8_t vio_header[VIO_HEADER_SIZE];
            load_vio_header(buf_dest[i], &vio_header);
            vio_header[0] += 1;
            store_vio_header(buf_dest[i], &vio_header);

#ifndef ZERO_COPY_TX
            auto copy_src_addr = get_packet_addr(&pool_guest->buffers[entry_idx]);
#if defined(SIMD_COPY_TX) || defined(NON_TEMPORAL_COPY_TX)
            void *xmm01 = get_packet_addr(buf_dest[i]);
            auto *xmm02 = copy_src_addr;
            for (int j = 0; j < NUM_LOOP; j++) {
#ifdef SIMD_COPY_TX
                _mm256_store_si256((__m256i *) xmm01 + j, _mm256_load_si256((__m256i *) xmm02 + j));
#else
                _mm256_stream_si256((__m256i *) xmm01 + j, _mm256_stream_load_si256((__m256i *) xmm02 + j));
#endif
            }
#else
            memcpy(get_packet_addr(buf_dest[i]), copy_src_addr, SIZE_PACKET);
#endif
            //cldemote(get_packet_addr(buf_dest[i]));
            //_mm_clflushopt(get_packet_addr(buf_dest[i]));
#endif
        }

        // Update index
        vq_tx->last_avail_idx++;
        vq_tx->last_avail_idx &= RING_SIZE_MASK;
    }

    for (int i = 0; i < num_fin; i++) {
        int8_t vio_header[VIO_HEADER_SIZE];
        load_vio_header(buf_dest[i], &vio_header);

#if BUF_HEADER_SIZE > 0
#ifdef READ_HEADER4_Tx
        if (*(int *) (buf_dest[i]->header.id_addr) == 999999) {
            exit(1);
        }
#elif defined(WRITE_HEADER4_Tx)
        memset(buf_dest[i]->header.id_addr, i, 4);
#else
        PROC_BUF_HEADER(buf_dest[i])
#endif
#endif
    }

#ifdef SKIP_INDEX_TX
    int skipped_index = 64 / VQ_ENTRY_SIZE;
    for (int i = skipped_index; i < num_fin; i++) {
#else
    for (int i = 0; i < num_fin; i++) {
#endif
        int desc_index = (last_avail_idx_shadow + i) & RING_SIZE_MASK;
        set_avail_flag(&vq_tx->descs[desc_index]);
    }

#ifdef SKIP_INDEX_TX
    for (int i = 0; i < skipped_index; i++) {
        int desc_index = (last_avail_idx_shadow + i) % VQ_ENTRY_NUM;
        set_avail_flag(&vq_tx->descs[desc_index]);
    }
#endif
}

void guest_recv_process(vq *vq_rx, guest_buffer_pool *pool, buf **pkts, int num_fin) {
    const uint16_t RING_SIZE_MASK = vq_rx->size - 1;

    wait_used(vq_rx, (vq_rx->last_avail_idx + num_fin - 1) & RING_SIZE_MASK);

    for (int i = 0; i < num_fin; i++) {
        int64_t index = vq_rx->descs[(vq_rx->last_avail_idx + i) & RING_SIZE_MASK].entry_index;
#if defined(READ_HEADER4_NF) || defined(WRITE_HEADER4_NF)
        PREFETCH_BUF(pool->buffers[index].header.id_addr, pool->buffers[index].header.id_addr)
#else
        PREFETCH_BUF(pool->buffers[index].header.id_addr, pool->buffers[index].header.len_addr)
#endif
        prefetch0(&vq_rx->descs[vq_rx->last_avail_idx + i]);
#if VIO_HEADER_SIZE > 0
        prefetch0(&pool->buffers[index].padding + VIO_HEADER_OFFSET);
#endif
    }

    int64_t id[num_fin];
    int last_avail_idx_shadow = vq_rx->last_avail_idx;

    for (int i = 0; i < num_fin; i++) {
        id[i] = vq_rx->descs[vq_rx->last_avail_idx].entry_index;
        pkts[i] = &pool->buffers[id[i]];

        int8_t vio_header[VIO_HEADER_SIZE];
        load_vio_header(pkts[i], &vio_header);

#if BUF_HEADER_SIZE > 0
#ifdef READ_HEADER4_NF
        if (*(int *) (pkts[i]->header.id_addr) == 999999) {
            exit(1);
        }
#elif defined(WRITE_HEADER4_NF)
        memset(pkts[i]->header.id_addr, i, 4);
#else
        set_id(pkts[i], vio_header[0]);
        set_len(pkts[i], SIZE_PACKET);
#endif
#endif

#ifdef READ_BODY4_NF
        if(get_packet_addr(pkts[i])->packet_id < 0) {
            exit(1);
        }
#elif defined(WRITE_BODY4_NF)
        get_packet_addr(pkts[i])->packet_id += 1;
#endif

        vq_rx->last_avail_idx++;
        vq_rx->last_avail_idx &= RING_SIZE_MASK;
    }

#ifdef SKIP_INDEX_NF
    int skipped_index = 64 / VQ_ENTRY_SIZE;
    for (int i = skipped_index; i < num_fin; i++) {
#else
    for (int i = 0; i < num_fin; i++) {
#endif
        int desc_idx = (last_avail_idx_shadow + i) & RING_SIZE_MASK;
#ifdef RANDOM_NF
        int next_buffer_index = pool->last_pool_idx / 32 * 32 + ids[i];
        pool->last_pool_idx = (pool->last_pool_idx + 1) % GUEST_POOL_ENTRY_NUM;
#else
        auto next_buffer_index = static_cast<int64_t>(get_buffer_index(pool, get_buffer(pool)));
#endif
#if BUF_HEADER_SIZE > 0 && !defined(READ_HEADER4_NF) && !defined(WRITE_HEADER4_NF) && !defined(RANDOM_NF) && GUEST_POOL_CACHE_ENTRIES > 0
        assert(get_len(&pool->buffers[next_buffer_index]) == -1);
#endif
        vq_rx->descs[desc_idx].entry_index = next_buffer_index;
        set_avail_flag(&vq_rx->descs[desc_idx]);
    }
}

void guest_flush_inflight_buffers(vq *vq_tx, guest_buffer_pool *pool) {
    const uint16_t RING_SIZE_MASK = vq_tx->size - 1;

    int n_free = vq_tx->last_used_idx - vq_tx->last_inflight_idx;
    if (n_free == 0) {
        return;
    } else if (n_free < 0) {
        n_free += vq_tx->size;
    }

    int desc_idx = vq_tx->last_used_idx - 1;
    if (desc_idx < 0) {
        desc_idx = vq_tx->size - 1;
    }

    wait_avail(vq_tx, desc_idx);

    for (int i = 0; i < n_free; i++) {
        int entry_idx = vq_tx->descs[vq_tx->last_inflight_idx].entry_index;
        add_to_cache(pool, &pool->buffers[entry_idx]);
#if BUF_HEADER_SIZE > 0 && !defined(READ_HEADER4_NF) && !defined(WRITE_HEADER4_NF)
        set_len(&pool->buffers[entry_idx], -1);
#endif

        vq_tx->last_inflight_idx++;
        vq_tx->last_inflight_idx &= RING_SIZE_MASK;
    }
}

void guest_send_process(vq *vq_tx, guest_buffer_pool *pool, buf **pkts, int num_fin) {
    const uint16_t RING_SIZE_MASK = vq_tx->size - 1;

    guest_flush_inflight_buffers(vq_tx, pool);

    wait_avail(vq_tx, (vq_tx->last_used_idx + num_fin - 1) & RING_SIZE_MASK);

    for (int i = 0; i < num_fin; i++) {
        prefetch0(&vq_tx->descs[vq_tx->last_used_idx + i]);
    }

    for (int i = 0; i < num_fin; i++) {
        vq_tx->descs[vq_tx->last_used_idx].entry_index = get_buffer_index(pool, pkts[i]);
        set_used_flag(&vq_tx->descs[vq_tx->last_used_idx]);

        vq_tx->last_used_idx++;
        vq_tx->last_used_idx &= RING_SIZE_MASK;
    }

#ifdef SKIP_INDEX_NF
    for (int i = 0; i < skipped_index; i++) {
        int tx_desc_entry = (last_used_idx_shadow + i) % VQ_ENTRY_NUM;
        set_used_flag(&vq_tx->descs[tx_desc_entry]);
        vq_tx->descs[tx_desc_entry].entry_index = id[i];

        int rx_desc_entry = (last_avail_idx_shadow + i) % VQ_ENTRY_NUM;
        set_avail_flag(&vq_rx->descs[rx_desc_entry]);
    }
#endif
}
