#pragma once
#include <universal/ptr32.h>
#include <universal/bo1_ptrcast.h>
#include <universal/bo1_layout.h>

#include <cstdint>
#include <windows.h>

#include <DemonWare/bdCore/bdReference/bdReferencable.h>

// ALL AISLOP, THIS DOESNT DESERVE ANY BETTER
// 
// ------------------------------------------------------------
// bdLinkedList<T>
// ------------------------------------------------------------

template <typename T>
struct bdLinkedList
{
    struct Node
    {
        // +0x00
        bdReference<T> m_value;

        // +0x0C
        Ptr32<Node> m_next;

        // +0x10
        Ptr32<Node> m_prev;
    };

    // +0x00
    Ptr32<Node> m_head;

    // +0x04
    Ptr32<Node> m_tail;

    // +0x08
    unsigned int m_size;

    // --------------------------------------------------------
    // ctor / dtor
    // --------------------------------------------------------

    inline bdLinkedList()
        : m_head(nullptr)
        , m_tail(nullptr)
        , m_size(0)
    {
    }

    inline ~bdLinkedList()
    {
        clear();
    }

    // --------------------------------------------------------
    // removeAt
    //  a2 = pointer to Node* slot holding the node to remove
    // --------------------------------------------------------

    inline Ptr32<Node> *removeAt(Ptr32<Node> *slot)
    {
        Node *node = *slot;
        if (!node)
            return slot;

        *slot = node->m_next;

        // unlink from head
        if (node == m_head)
            m_head = node->m_next;
        else
            node->m_prev->m_next = node->m_next;

        // unlink from tail
        if (node == m_tail)
            m_tail = node->m_prev;
        else
            node->m_next->m_prev = node->m_prev;

        // destroy payload + free node
        node->m_value.~bdReference<T>();
        bdMemory::deallocate(node);

        --m_size;
        return &m_head; // (the original returns the list itself, whose first member is the head slot)
    }

    // --------------------------------------------------------
    // insertAfter
    //  a2 = Node after which to insert (null = tail insert)
    //  a3 = bdReference<T> source
    // --------------------------------------------------------

    inline int *insertAfter(Node *after, const bdReference<T> *value)
    {
        Node *node = static_cast<Node *>(bdMemory::allocate(sizeof(Node)));
        if (!node)
            return reinterpret_cast<int *>(this);

        // copy bdReference<T> (a new reference: count it)
        node->m_value.m_ptr = value->m_ptr;
        if (node->m_value.m_ptr)
            InterlockedIncrement(&node->m_value.m_ptr->m_refCount);

        new (&node->m_value + 1) bdSecurityID(
            reinterpret_cast<const bdSecurityID *>(
                reinterpret_cast<const char *>(value) + 4));

        // insertion logic
        if (after)
        {
            node->m_next = after->m_next;
            node->m_prev = after;

            if (after->m_next)
                after->m_next->m_prev = node;
            else
                m_tail = node;

            after->m_next = node;
        }
        else
        {
            node->m_next = nullptr;
            node->m_prev = m_tail;

            if (m_tail)
                m_tail->m_next = node;
            else
                m_head = node;

            m_tail = node;
        }

        ++m_size;
        return reinterpret_cast<int *>(this);
    }

    // --------------------------------------------------------
    // clear
    // --------------------------------------------------------

    inline int clear()
    {
        while (m_head)
        {
            Node *next = m_head->m_next;

            m_head->m_value.~bdReference<T>();
            bdMemory::deallocate(m_head);

            m_head = next;
        }

        m_tail = nullptr;
        m_size = 0;
        return reinterpret_cast<int>(BO1_P2I(this));
    }
};

BO1_LAYOUT32_ASSERT(sizeof(bdLinkedList<void *>) == 0x0C, "bdLinkedList size mismatch");
