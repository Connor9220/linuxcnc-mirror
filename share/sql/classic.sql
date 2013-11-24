<schema>
BEGIN TRANSACTION;

DROP TABLE IF EXISTS tools;
DROP TABLE IF EXISTS offsets;
DROP TABLE IF EXISTS geometries;
DROP TABLE IF EXISTS geom_groups;
DROP TABLE IF EXISTS magazines;
DROP TABLE IF EXISTS spindles;
DROP TABLE IF EXISTS pockets;
DROP TABLE IF EXISTS mag_types;
   
CREATE TABLE tools (
    pocket      INTEGER PRIMARY KEY,
    tool        INTEGER,
    description TEXT DEFAULT "",
    X           REAL DEFAULT 0.0,
    Y           REAL DEFAULT 0.0,
    Z           REAL DEFAULT 0.0,
    A           REAL DEFAULT 0.0,
    B           REAL DEFAULT 0.0,
    C           REAL DEFAULT 0.0,
    U           REAL DEFAULT 0.0,
    V           REAL DEFAULT 0.0,
    W           REAL DEFAULT 0.0,
    diameter    REAL DEFAULT 0.0,
    orientation INTEGER DEFAULT NULL,
    frontangle  REAL DEFAULT NULL,
    backangle   REAL DEFAULT NULL,
    spindle     INTEGER DEFAULT 0
);

COMMIT;
</schema>

<import>
/*CLASSIC IMPORT SCRIPT. Emulates EMC2 style offsets
Valid tags are :Tool :Pocket :X :Y :Z :U :V: :W :A :B :C 
 :Diameter, :Frontangle :Backangle :Orientation :Comment: */
BEGIN TRANSACTION;
INSERT INTO tools (pocket, tool, description, X, Y, Z, A, B, C, U, V, W, 
                   diameter, orientation, frontangle, backangle) 
           VALUES (:Pocket, :Tool, ":Comment", :X, :Y, :Z, :A, :B, :C,
                   :U, :V, :W, :Diameter,
                   :Orientation, :Frontangle, :Backangle);
                   
COMMIT;
</import>

<find_tool>
select tool'toolID', pocket'pocket' from tools where tool = :T;
</find_tool>

<get_offset>
select X, Y, Z, A, B, C, U, V, W, diameter from tools where tool = :H ;
</get_offset>

<get_spindle_tool>
select tool'toolID' from tools where spindle > 0 ;
</get_spindle_tool>

<set_spindle_tool>
update tools
set spindle = case when tool = :T then :S else 0 end ;
</set_spindle_tool>


<set_tool_pocket>
update tools
set pocket = :P where tool = :T ;
</set_tool_pocket>

<get_pocket_by_tool>
select pocket'pocket' from tools where tool = :T ;
</get_pocket_by_tool>

<get_tool_by_pocket>
select tool'toolID' from tools where pocket = :P ;
</get_tool_by_pocket>

