/*
 * Copyright (c) 2020  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.model.db

import android.content.Context
import androidx.room.AutoMigration
import androidx.room.Database
import androidx.room.ProvidedAutoMigrationSpec
import androidx.room.DeleteColumn
import androidx.room.RenameColumn
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.room.migration.AutoMigrationSpec
import androidx.sqlite.SQLiteConnection
import androidx.sqlite.execSQL
import androidx.sqlite.db.SupportSQLiteDatabase
import com.gaurav.avnc.model.ServerProfile

@Database(entities = [ServerProfile::class], version = MainDb.VERSION, exportSchema = true, autoMigrations = [
    AutoMigration(from = 1, to = 2, spec = MainDb.MigrationSpec1to2::class),  // in v2.0.0
    AutoMigration(from = 2, to = 3, spec = MainDb.MigrationSpec2to3::class),  // in v2.1.0
    AutoMigration(from = 3, to = 4),                                          // in v2.2.2
    AutoMigration(from = 4, to = 5, spec = MainDb.MigrationSpec4to5::class),  // in v2.3.0
    AutoMigration(from = 5, to = 6),                                          // in v2.x.x
    AutoMigration(from = 6, to = 7, spec = MainDb.MigrationSpec6to7::class),  // in v3.0.0
    AutoMigration(from = 7, to = 8),                                          // macOS display options
    AutoMigration(from = 8, to = 9, spec = MainDb.EncryptCredentials::class),
])
abstract class MainDb : RoomDatabase() {
    abstract val serverProfileDao: ServerProfileDao

    companion object {
        /**
         * Current database version
         */
        const val VERSION = 9

        private var instance: MainDb? = null

        /**
         * Returns database singleton.
         * If database is not yet created then it will be created on first call.
         */
        @Synchronized
        fun getInstance(context: Context): MainDb {
            if (instance == null) {
                val secrets = ProfileSecrets(context)
                instance = Room.databaseBuilder(context, MainDb::class.java, "main")
                        .addTypeConverter(secrets)
                        .addAutoMigrationSpec(EncryptCredentials(secrets))
                        .addCallback(object : Callback() {
                            override fun onOpen(db: SupportSQLiteDatabase) {
                                val cleanup = db.query("SELECT 1 FROM sqlite_master WHERE name='avnc_secret_cleanup'")
                                        .use { it.moveToFirst() }
                                if (cleanup) {
                                    db.execSQL("VACUUM")
                                    val busy = db.query("PRAGMA wal_checkpoint(TRUNCATE)").use { it.moveToFirst() && it.getInt(0) != 0 }
                                    if (!busy) db.execSQL("DROP TABLE avnc_secret_cleanup")
                                }
                            }
                        }).build()
            }
            return instance!!
        }
    }

    /******************************** Migrations ***********************************/
    // Added in v2.0.0
    class MigrationSpec1to2 : AutoMigrationSpec {
        override fun onPostMigrate(db: SupportSQLiteDatabase) {
            db.execSQL("UPDATE profiles SET imageQuality = 5")
        }
    }

    // Added in v2.1.0
    @RenameColumn(tableName = "profiles", fromColumnName = "keyCompatMode", toColumnName = "compatFlags")
    class MigrationSpec2to3 : AutoMigrationSpec

    // Added in v2.3.0
    @RenameColumn(tableName = "profiles", fromColumnName = "compatFlags", toColumnName = "flags")
    @RenameColumn(tableName = "profiles", fromColumnName = "shortcutRank", toColumnName = "useCount")
    class MigrationSpec4to5 : AutoMigrationSpec

    // Added in v3.0.0
    @RenameColumn(tableName = "profiles", fromColumnName = "viewOnly", toColumnName = "viewMode")
    @DeleteColumn(tableName = "profiles", columnName = "sshPrivateKeyPassword")
    class MigrationSpec6to7 : AutoMigrationSpec

    @ProvidedAutoMigrationSpec
    class EncryptCredentials(private val secrets: ProfileSecrets) : AutoMigrationSpec {
        override fun onPostMigrate(connection: SQLiteConnection) {
            // SQLite retains the old TEXT values when Room copies them into BLOB columns.
            // Read and replace them inside Room's upgrade transaction.
            connection.prepare("PRAGMA secure_delete=ON").use { it.step() }
            connection.prepare("UPDATE profiles SET password=?, sshPassword=?, sshPrivateKey=? WHERE ID=?").use { update ->
                connection.prepare("SELECT ID, password, sshPassword, sshPrivateKey FROM profiles ORDER BY ID").use { rows ->
                    while (rows.step()) {
                        for (column in 1..3) update.bindBlob(column, secrets.encrypt(rows.getText(column)))
                        update.bindLong(4, rows.getLong(0))
                        update.step()
                        update.reset()
                    }
                }
            }
            // The upgrade commits before onOpen can reclaim old pages and truncate the journal.
            connection.execSQL("CREATE TABLE avnc_secret_cleanup (pending INTEGER)")
        }
    }
}
