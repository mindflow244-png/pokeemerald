#ifndef GUARD_RT_BATTLE_H
#define GUARD_RT_BATTLE_H

// Combate en tiempo real (ATB) sobre el mapa. Etapa S1 del diseno en rt-design/.
void RT_Tick(u16 newKeys);
bool8 RT_IsActive(void);
bool8 RT_ShouldBlockInteraction(void);

#endif // GUARD_RT_BATTLE_H
