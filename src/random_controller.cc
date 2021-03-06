/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include <string.h>
#include <iostream>
#include <fstream>
#include <ns3/random-variable.h>
#include <ns3/log.h>

#include "controller.h"
#include "loadbalancer.h"

namespace ns3 {

    namespace ofi {

        NS_LOG_COMPONENT_DEFINE ("random");


        RandomizeController::RandomizeController(int server_number)
        {
            num_servers = server_number;
        }

        void RandomizeController::ReceiveFromSwitch(Ptr<OpenFlowSwitchNetDevice> swtch, ofpbuf* buffer)
        {
            //  LogComponentEnable("random",LOG_LEVEL_LOGIC);
            // Comprueba que el switch está registrado conrrectamente con el controller
            if (m_switches.find(swtch) == m_switches.end())
            {
                NS_LOG_ERROR ("Imposible realizar la conexión Switch<->Controller");
                return;
            }

            if(num_servers == 0)
            {
                num_servers = OF_DEFAULT_SERVER_NUMBER;
            }

            // Determina qué tipo de paquete es el recibido en el buffer
            uint8_t type = GetPacketType(buffer);

            if (type == OFPT_PACKET_IN)  // Paquete OpenFlow de entrada en el cotroller
            {
                NS_LOG_LOGIC ("Tipo == OFPT_PACKET_IN");

                // opi (Openflow Packet In) almacena el paquete de entrada
                ofp_packet_in * opi = (ofp_packet_in*)ofpbuf_try_pull(buffer, offsetof(ofp_packet_in, data));
                int port = ntohs(opi->in_port);				// Almacena el puerto por el que se ha recibido la trama

                /* OpenFlow asigna una clave (key) a cada flujo, lo cual permite identificarlo y actuar sobre él
                A continuación se extrae esta clave */
                sw_flow_key key;
                key.wildcards = 0;
                flow_extract(buffer, port != -1 ? port : OFPP_NONE, &key.flow);

                // Obtiene el puerto del switch por el que se ha recibido y defne otros valores para puertos que se usarán posteriormente


                //			|-> Convierte el formato: ntohs (Network TO Host Short)
                uint16_t out_port = OFPP_FLOOD;					// Puerto de salida del flujo
                uint16_t in_port = ntohs(key.flow.in_port); 	// Puerto de entrada del flujo
                NS_LOG_LOGIC("Puerto de llegada = " << in_port);

                /* Direcciones MAC del flujo */
                Mac48Address src_macAddr, dst_macAddr;	// Dirección MAC origen y destino
                src_macAddr.CopyFrom(key.flow.dl_src);	// Dirección MAC origen del flujo
                dst_macAddr.CopyFrom(key.flow.dl_dst);	// Dirección MAC destino del flujo
                NS_LOG_LOGIC ("MAC src = " << src_macAddr);
                NS_LOG_LOGIC ("MAC dst = " << dst_macAddr);

                /* Direcciones IP del flujo */
                Ipv4Address src_ipv4Addr(ntohl(key.flow.nw_src));	// Dirección IP origen del flujo
                Ipv4Address dst_ipv4Addr(ntohl(key.flow.nw_dst));	// Dirección IP destino del flujo
                Ipv4Address server_ipv4Addr("172.16.0.1");			// Dirección IP de los servidores
                NS_LOG_LOGIC ("IP src = " << src_ipv4Addr);
                NS_LOG_LOGIC ("IP dst = " << dst_ipv4Addr);

                // Aprende y rellena la tabla de asociación [MAC->Puerto]
                LearnState_t::iterator st = m_learnState.find(src_macAddr); // Busca MAC_src obtenida del flujo en la tabla del switch
                if (st == m_learnState.end()) 	// Si no está de antes, se aprende ahora
                {
                    LearnedState ls;
                    ls.port = in_port;
                    m_learnState.insert(std::make_pair(src_macAddr, ls));
                    NS_LOG_INFO ("Aprendida MAC = " << src_macAddr << " -> Puerto = " << in_port);
                }

                // Comprueba si es un mensaje ARP probe
                bool isArpProbe = false;
                if (src_ipv4Addr.IsEqual(src_ipv4Addr.GetZero()))
                {
                    NS_LOG_LOGIC ("Paquete ARP");
                    isArpProbe = true;
                }
                else
                {
                    NS_LOG_LOGIC ("Paquete normal");
                }

                // Comprueba si es un mensaje de difusión a nivel MAC
                if (dst_macAddr.IsBroadcast ())
                {
                    NS_LOG_LOGIC ("Paquete difusión MAC");
                    if (isArpProbe)	// Paquete ARP probe, hay que elegir quién responde
                    {
                        NS_LOG_LOGIC ("Paquete ARP");
                        /* Se prepara el generador aleatorio que determina qué servidor responderá al mensaje ARP, recordar que todos los servidores
                        tienen la misma dirección IP, pero la MAC es diferente, por eso solo debe responder uno. En este modo se determina cuál es
                        el que responde de manera aleatoria.
                        */
                        uint32_t min = 0;						// Inicio del random
                        uint32_t max = num_servers;				// Fin del random igual al nº de servidores
                        UniformVariable uv;						// Variable aleatoria
                        out_port = uv.GetInteger(min, max);		// Puerto de salida del flujo (hacia el servidor que ha tocado aleatoriamente)
                    }
                    else 			// Difusión que no es ARP probe => flood
                    {
                        NS_LOG_LOGIC ("Paquete difusión MAC no ARP => flood");
                        out_port = OFPP_FLOOD;
                    }
                }
                else
                {
                    NS_LOG_LOGIC ("Paquete NO difusión MAC");
                    /*  Si no es un mensaje de difusión...
                    Actuamos como un switch normal:
                    Mira la tabla [MAC->Puerto] que tiene el switch para ver si se puede averiguar qué hacer con este paquete
                    */
                    LearnState_t::iterator st = m_learnState.find(dst_macAddr); // Busca MAC_dst obtenida del flujo en la tabla del switch
                    if (st != m_learnState.end())
                    {
                        // Coincidencia en la tabla => Puerto de salida = puerto que ha hecho match de la tabla
                        out_port = st->second.port;
                        NS_LOG_LOGIC ("Match en la tabla! MAC = " << dst_macAddr << " -> Puerto = " << out_port);
                    }
                    else
                    {
                        // No tenemos ni idea de qué hacer con este flujo => flood (envío por todos los puertos excepto por donde llegó)
                        out_port=OFPP_FLOOD;
                        NS_LOG_LOGIC ("MAC nunca registrada => flood");
                    }
                }

                // En este punto ya tenemos identificados los participantes en el flujo y su dirección, ahora el controller debe dar las órdenes al switch para que actúe
                NS_LOG_LOGIC ("Servidor (puerto) elegido = " << out_port);
                ofp_action_output act[1];
                act[0].type = htons(OFPAT_OUTPUT);				// Tipo de acción = Salida del switch
                act[0].len = htons(sizeof(ofp_action_output));	// Longitud
                act[0].port = out_port;  						// Puerto de salida, el determinado en el algoritmo anterior

                // Se crea el nuevo mensaje asociado al flujo desde el cliente hasta el servidor, incluyendo los datos generados por el algoritmo
                ofp_flow_mod* ofm = BuildFlow(key, opi->buffer_id, OFPFC_ADD, act, sizeof(act), OFP_FLOW_PERMANENT, OFP_FLOW_PERMANENT);
                /*							   |			|			|	   |		|				|				|-> hard_timeout: Desactivado
                |			|			|	   |		|				|->idle_timeout: Desactivado
                |			|			|	   |		|->	actions_len: Longitud de la lista de acciones
                |			|			|	   |-> acts: Lista de acciones para ejecutar
                |			|			|-> command: Acción (ADD, MODIFY o DELETE) sobre el flujo
                |			|-> buffer_id: Para ejecutar las acciones sobre el paquet esi se borra o modifica el flujo
                |-> key: Utilizado para crear un flujo que coincida con el paquete
                */

                // Envía el mensaje al switch asociado a este controller
                SendToSwitch(swtch, ofm, ofm->header.length);
                /*				|	  |			|-> length: Longitud del mensaje
                | 	  |-> msg: Mensaje a enviar (flujo creado anteriormente)
                |-> swtch: Switch asociado y receptor de este mensaje
                */
            }
            return;
        }
    }
}
