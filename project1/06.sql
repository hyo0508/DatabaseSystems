SELECT T.name, AVG(C.level) as avg_level
FROM Trainer as T, Gym as G, CatchedPokemon as C
WHERE T.id = G.leader_id AND
	T.id = C.owner_id
GROUP BY T.name
ORDER BY T.name;