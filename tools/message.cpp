//
//    ALIO - ALternative IO library
//    Copyright (C) 2013  Joerg Henrichs
//
//    ALIO is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    ALIO is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with ALIO.  If not, see <http://www.gnu.org/licenses/>.
//

#include "tools/i_communication.hpp"
#include "tools/message.hpp"

#include <string>
#include <math.h>
#include <stdexcept>
#include <assert.h>
#include <stdio.h>

/** Creates a message to be sent.
 *  This only initialised the data structures, it does not reserve any memory.
 *  A call to allocate() is therefore necessary.
 *  \param type  The type of the message
 */
Message::Message(MessageType type, int index)
{
    assert(sizeof(int)==4);
    m_index         = index;
    m_type          = type;
    m_pos           = 0;
    m_data_size     = -1;
    m_data          = NULL;
    m_needs_destroy = false;
}   // Message

// ----------------------------------------------------------------------------
/** Handles a received message.
 *  \param type The type of the message. Used in asserts to detect errors.
 *  \param buffer The actual data.
 *  \param len Amount of data in bytes.
 */

Message::Message(MessageType type, const void *buffer, int len)
{
    assert(sizeof(int)==4);
    m_data_size = len;
    m_data      = (char*)buffer;
    m_type      = type;
    m_pos       = 0;
    m_needs_destroy = true;
    MessageType message_type;
    get(&message_type);
    assert(message_type==type);
    get(&m_index);
}   // Message(type, buffer, len);

// ----------------------------------------------------------------------------
/** Frees the memory allocated for this message. */
Message::~Message()
{
    clear();
}   // ~Message

// ----------------------------------------------------------------------------
/** Frees the memory for a received message.
 *  Calls enet_packet_destroy if necessary (i.e. if the message was received).
 *  The memory for a message created to be sent does not need to be freed, it 
 *  is handled by enet. */
void Message::clear()
{
    if(m_needs_destroy)
        delete [] m_data;

    m_data = NULL;
    m_needs_destroy = false;
}   // clear

// ----------------------------------------------------------------------------
/** Reserves the memory for a message. 
 *  \param size Number of bytes to reserve.
 */
void Message::allocate(size_t size)
{
    // Message type and local file index at the beginning
    m_data_size = sizeof(m_type)+sizeof(m_index)+size;
    m_data      = new char[m_data_size];
    if(!m_data)
    {
        printf("can't allocate '%d' bytes.\n", m_data_size);
        assert(false);
    }
    add(m_type);
    add(m_index);
    m_needs_destroy = true;
}   // allocate

// ----------------------------------------------------------------------------
void Message::send(ICommunication *communication)
{
    communication->send(getData(), getLen(), /*tag*/9);
}   // send

// ----------------------------------------------------------------------------

